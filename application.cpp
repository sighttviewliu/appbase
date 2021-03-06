#include <appbase/application.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/asio/signal_set.hpp>

#include <iostream>
#include <fstream>

namespace appbase {

namespace bpo = boost::program_options;
using bpo::options_description;
using bpo::variables_map;
using std::cout;

class application_impl {
   public:
      application_impl():_app_options("Application Options"){
      }
      const variables_map*    _options = nullptr;
      options_description     _app_options;
      options_description     _cfg_options;

      bfs::path                _data_dir;
};

application::application()
:my(new application_impl()){
   io_serv = std::make_shared<boost::asio::io_service>();
}

application::~application() { }

void application::startup() {
   for (auto plugin : initialized_plugins)
      plugin->startup();
}

application& application::instance() {
   static application _app;
   return _app;
}
application& app() { return application::instance(); }


void application::set_program_options()
{
   for(auto& plug : plugins) {
      boost::program_options::options_description plugin_cli_opts("Command Line Options for " + plug.second->name());
      boost::program_options::options_description plugin_cfg_opts("Config Options for " + plug.second->name());
      plug.second->set_program_options(plugin_cli_opts, plugin_cfg_opts);
      if(plugin_cfg_opts.options().size()) {
         my->_app_options.add(plugin_cfg_opts);
         my->_cfg_options.add(plugin_cfg_opts);
      }
      if(plugin_cli_opts.options().size())
         my->_app_options.add(plugin_cli_opts);
   }

   options_description app_cfg_opts( "Application Config Options" );
   options_description app_cli_opts( "Application Command Line Options" );
   app_cfg_opts.add_options()
         ("plugin", bpo::value< vector<string> >()->composing(), "Plugin(s) to enable, may be specified multiple times");

   app_cli_opts.add_options()
         ("help,h", "Print this help message and exit.")
         ("version,v", "Print version information.")
         ("data-dir,d", bpo::value<bfs::path>()->default_value( "data-dir" ), "Directory containing configuration file config.ini")
         ("config,c", bpo::value<bfs::path>()->default_value( "config.ini" ), "Configuration file name relative to data-dir");

   my->_cfg_options.add(app_cfg_opts);
   my->_app_options.add(app_cfg_opts);
   my->_app_options.add(app_cli_opts);
}

bool application::initialize_impl(int argc, char** argv, vector<abstract_plugin*> autostart_plugins) {
   set_program_options();

   bpo::variables_map options;
   bpo::store(bpo::parse_command_line(argc, argv, my->_app_options), options);

   if( options.count( "help" ) ) {
      cout << my->_app_options << "\n";
      return false;
   }

   bfs::path data_dir = "data-dir";
   if( options.count("data-dir") )
   {
      data_dir = options["data-dir"].as<bfs::path>();
      if( data_dir.is_relative() )
         data_dir = bfs::current_path() / data_dir;
   }
   my->_data_dir = data_dir;

   bfs::path config_file_name = data_dir / "config.ini";
   if( options.count( "config" ) ) {
      auto config_file_name = options["config"].as<bfs::path>();
      if( config_file_name.is_relative() )
         config_file_name = data_dir / config_file_name;
   }

   if(!bfs::exists(config_file_name)) {
      write_default_config(config_file_name);
   }

   bpo::store(bpo::parse_config_file<char>(config_file_name.make_preferred().string().c_str(),
                                           my->_cfg_options, true), options);

   if(options.count("plugin") > 0)
   {
      auto plugins = options.at("plugin").as<std::vector<std::string>>();
      for(auto& arg : plugins)
      {
         vector<string> names;
         boost::split(names, arg, boost::is_any_of(" \t,"));
         for(const std::string& name : names)
            get_plugin(name).initialize(options);
      }
   }
   for (auto plugin : autostart_plugins)
      if (plugin != nullptr && plugin->get_state() == abstract_plugin::registered)
         plugin->initialize(options);

   bpo::notify(options);

   return true;
}

void application::shutdown() {
   for(auto ritr = running_plugins.rbegin();
       ritr != running_plugins.rend(); ++ritr) {
      (*ritr)->shutdown();
   }
   for(auto ritr = running_plugins.rbegin();
       ritr != running_plugins.rend(); ++ritr) {
      plugins.erase((*ritr)->name());
   }
   running_plugins.clear();
   initialized_plugins.clear();
   plugins.clear();
}

void application::quit() {
   io_serv->stop();
}

void application::exec() {
   std::shared_ptr<boost::asio::signal_set> sigint_set(new boost::asio::signal_set(*io_serv, SIGINT));
   sigint_set->async_wait([sigint_set,this](const boost::system::error_code& err, int num) {
     quit();
     sigint_set->cancel();
   });

   std::shared_ptr<boost::asio::signal_set> sigterm_set(new boost::asio::signal_set(*io_serv, SIGTERM));
   sigterm_set->async_wait([sigterm_set,this](const boost::system::error_code& err, int num) {
     quit();
     sigterm_set->cancel();
   });

   io_serv->run();

   shutdown(); /// perform synchronous shutdown
}

void application::write_default_config(const bfs::path& cfg_file) {
   if(!bfs::exists(cfg_file.parent_path()))
      bfs::create_directories(cfg_file.parent_path());

   std::ofstream out_cfg( bfs::path(cfg_file).make_preferred().string());
   for(const boost::shared_ptr<bpo::option_description> od : my->_cfg_options.options())
   {
      if(!od->description().empty())
         out_cfg << "# " << od->description() << "\n";
      boost::any store;
      if(!od->semantic()->apply_default(store))
         out_cfg << "# " << od->long_name() << " = \n";
      else {
         auto example = od->format_parameter();
         if(example.empty())
            // This is a boolean switch
            out_cfg << od->long_name() << " = " << "false\n";
         else {
            // The string is formatted "arg (=<interesting part>)"
            example.erase(0, 6);
            example.erase(example.length()-1);
            out_cfg << od->long_name() << " = " << example << "\n";
         }
      }
      out_cfg << "\n";
   }
   out_cfg.close();
}

abstract_plugin* application::find_plugin(const string& name)const
{
   auto itr = plugins.find(name);
   if(itr == plugins.end()) {
      return nullptr;
   }
   return itr->second.get();
}

abstract_plugin& application::get_plugin(const string& name)const {
   auto ptr = find_plugin(name);
   if(!ptr)
      BOOST_THROW_EXCEPTION(std::runtime_error("unable to find plugin: " + name));
   return *ptr;
}

bfs::path application::data_dir()const {
   return my->_data_dir;
}

} /// namespace appbase
