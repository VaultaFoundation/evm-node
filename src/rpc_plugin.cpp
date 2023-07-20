#include "rpc_plugin.hpp"

#include <iostream>
#include <string>

#include <silkworm/core/chain/config.hpp>
#include <silkworm/node/common/settings.hpp>
#include <silkworm/silkrpc/settings.hpp>
#include <silkworm/silkrpc/daemon.hpp>

// #include <agrpc/grpc_context.hpp>
// #include <grpcpp/grpcpp.h>
// #include <silkworm/core/types/log.hpp>
// #include <silkworm/common/settings.hpp>
// #include <silkworm/common/log.hpp>

class rpc_plugin_impl : std::enable_shared_from_this<rpc_plugin_impl> {
   public:
      rpc_plugin_impl(silkworm::rpc::DaemonSettings settings)
         : settings(settings) {}

      void init(const silkworm::rpc::DaemonSettings& s) {
         settings = s;
      }

      silkworm::rpc::DaemonSettings settings;
      std::thread daemon_thread;
};

rpc_plugin::rpc_plugin() {}
rpc_plugin::~rpc_plugin() {}

void rpc_plugin::set_program_options( appbase::options_description& cli, appbase::options_description& cfg ) {
   cfg.add_options()
      ("http-port", boost::program_options::value<std::string>()->default_value(kDefaultEth1EndPoint),
        "http port for JSON RPC of the form <address>:<port>")    
      ("rpc-engine-port", boost::program_options::value<std::string>()->default_value(kDefaultEngineEndPoint),
        "engine port for JSON RPC of the form <address>:<port>")
      ("eos-evm-node", boost::program_options::value<std::string>()->default_value(kDefaultPrivateApiAddr),
        "address to eos-evm-node of the form <address>:<port>")
      ("rpc-threads", boost::program_options::value<uint32_t>()->default_value(16),
        "number of threads for use with rpc")
      ("chaindata", boost::program_options::value<std::string>()->default_value("./"),
        "directory of chaindata")
      ("rpc-max-readers", boost::program_options::value<uint32_t>()->default_value(16),
        "maximum number of rpc readers")
      ("api-spec", boost::program_options::value<std::string>()->default_value("eth"),
        "comma separated api spec, possible values: debug,engine,eth,net,parity,erigon,txpool,trace,web3")
      ("chain-id", boost::program_options::value<uint32_t>()->default_value(silkworm::kEOSEVMLocalTestnetConfig.chain_id),
        "override chain-id")
   ;
}

// silkworm::rpc::LogLevel to_silkrpc_log_level(silkworm::log::Level v) {
//    switch (v) {
//       case silkworm::log::Level::kNone:
//          return silkworm::rpc::LogLevel::None;
//       case silkworm::log::Level::kCritical:
//          return silkworm::rpc::LogLevel::Critical;
//       case silkworm::log::Level::kError:
//          return silkworm::rpc::LogLevel::Error;
//       case silkworm::log::Level::kWarning:
//          return silkworm::rpc::LogLevel::Warn;
//       case silkworm::log::Level::kInfo:
//          return silkworm::rpc::LogLevel::Info;
//       case silkworm::log::Level::kDebug:
//          return silkworm::rpc::LogLevel::Debug;
//       case silkworm::log::Level::kTrace:
//          return silkworm::rpc::LogLevel::Trace;
//       default:
//          break;
//    }

//    std::string err = "Unknown silkworm log level: ";
//    err += std::to_string(static_cast<int64_t>(v));
//    throw std::runtime_error(err);
// }

void rpc_plugin::plugin_initialize( const appbase::variables_map& options ) try {

   const auto& http_port   = options.at("http-port").as<std::string>();
   const auto& engine_port   = options.at("rpc-engine-port").as<std::string>();
   const auto  threads     = options.at("rpc-threads").as<uint32_t>();
   const auto  max_readers = options.at("rpc-max-readers").as<uint32_t>();

   // TODO when we resolve issues with silkrpc compiling in eos-evm-node then remove 
   // the `eos-evm-node` options and use silk_engine for the address and configuration
   const auto& node_port  = options.at("eos-evm-node").as<std::string>();
   //const auto node_settings   = engine.get_node_settings();
   const auto& data_dir   = options.at("chaindata").as<std::string>();

   auto log_level = appbase::app().get_plugin<sys_plugin>().get_verbosity();
   using evmc::operator""_bytes32;
   
   uint32_t chain_id = options.at("chain-id").as<uint32_t>();
   const auto chain_info = silkworm::lookup_known_chain(chain_id);
   if (!chain_info) {
      throw std::runtime_error{"unknown chain ID: " + std::to_string(chain_id)};
   }
   silkworm::ChainConfig config = *(chain_info->second);
   
   silkworm::NodeSettings node_settings;
   node_settings.data_directory = std::make_unique<silkworm::DataDirectory>(data_dir, false);
   node_settings.network_id = config.chain_id;
   node_settings.etherbase  = silkworm::to_evmc_address(silkworm::from_hex("").value()); // TODO determine etherbase name
   node_settings.chaindata_env_config = {node_settings.data_directory->chaindata().path().string(), false, true, false, false, true};

   //  bool create{false};          // Whether db file must be created
   //  bool readonly{false};        // Whether db should be opened in RO mode
   //  bool exclusive{false};       // Whether this process has exclusive access
   //  bool inmemory{false};        // Whether this db is in memory
   //  bool shared{false};          // Whether this process opens a db already opened by another process
   //  bool read_ahead{false};      // Whether to enable mdbx read ahead
   //  bool write_map{false};       // Whether to enable mdbx write map

   node_settings.chaindata_env_config.max_readers = max_readers;
   node_settings.chain_config = config;

   silkworm::log::Settings log_settings{
      .log_verbosity = log_level
   };

   silkworm::rpc::DaemonSettings settings{
      .log_settings          = silkworm::log::Settings{
         .log_verbosity = log_level
      },
      .context_pool_settings = silkworm::concurrency::ContextPoolSettings{},
      .datadir               = data_dir,
      .eth_end_point         = http_port,
      .engine_end_point      = engine_port,
      .eth_api_spec          = options.at("api-spec").as<std::string>(),
      .private_api_addr      = node_port,
      .num_workers           = threads,
      .skip_protocol_check   = true
   };

   my.reset(new rpc_plugin_impl(settings));

   SILK_INFO << "Initialized RPC Plugin";
} catch (const std::exception &ex) {
   SILK_ERROR << "Failed to initialize RPC Plugin, " << ex.what();
   throw;
} catch (...) {
   SILK_ERROR << "Failed to initialize RPC Plugin with unknown reason";
   throw;
}

void rpc_plugin::plugin_startup() {
   my->daemon_thread = std::thread([this]() {
      silkworm::log::set_thread_name("rpc-daemon");
      silkworm::rpc::Daemon::run(my->settings, {"eos-evm-rpc", "version: "+appbase::app().full_version_string()});
   });
}

void rpc_plugin::plugin_shutdown() {
   if (my->daemon_thread.joinable()) {
      my->daemon_thread.join();
   }
}