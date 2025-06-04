#include "ship_receiver_plugin.hpp"
#include "abi_utils.hpp"
#include "utils.hpp"

#include <string>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/program_options.hpp>

namespace asio      = boost::asio;
namespace websocket = boost::beast::websocket;

using asio::ip::tcp;
using boost::beast::flat_buffer;
using boost::system::error_code;

using sys = sys_plugin;
class ship_receiver_plugin_impl : std::enable_shared_from_this<ship_receiver_plugin_impl> {
   public:
      ship_receiver_plugin_impl()
         : native_blocks_channel(appbase::app().get_channel<channels::native_blocks>()) {
         }

      using block_result_t = eosio::ship_protocol::get_blocks_result_v0;
      using native_block_t = channels::native_block;

      void init(std::string h, std::string p, eosio::name ca, std::optional<uint64_t> input_start_height,
                uint32_t input_max_retry, uint32_t input_delay_second) {
         SILK_DEBUG << "ship_receiver_plugin_impl INIT";
         host = std::move(h);
         port = std::move(p);
         core_account = ca;
         last_lib = 0;
         last_block_num = 0;
         delay_second = input_delay_second;
         max_retry = input_max_retry;
         retry_count = 0;
         resolver = std::make_shared<tcp::resolver>(appbase::app().get_io_context());
         start_from_canonical_height = input_start_height;
         // Defer connection to plugin_start()
      }

      void shutdown() {
      }

      auto send_request(const eosio::ship_protocol::request& req) {
         auto bin = eosio::convert_to_bin(req);
         boost::system::error_code ec;
         stream->write(asio::buffer(bin), ec);
         if (ec) {
            SILK_ERROR << "Sending request failed : " << ec.message();
         }
         return ec;
      }

      auto connect_stream(auto& resolve_it) {
         boost::system::error_code ec;
         boost::asio::connect(stream->next_layer(), resolve_it, ec);

         if (ec) {
            SILK_ERROR << "SHiP connection failed : " << ec.message();
            return ec;
         }

         stream->handshake(host, "/", ec);

         if (ec) {
            SILK_ERROR << "SHiP failed handshake : " << ec.message();
            return ec;
         }

         SILK_INFO << "Connected to SHiP at " << host << ":" << port;
         return ec;
      }

      auto initial_read() {
         flat_buffer buff;
         boost::system::error_code ec;
         stream->read(buff, ec);
         if (ec) {
            SILK_ERROR << "SHiP initial read failed : " << ec.message();
         }
         auto end = buff.prepare(1);
         ((char *)end.data())[0] = '\0';
         buff.commit(1);
         abi = load_abi(eosio::json_token_stream{(char *)buff.data().data()});
         return ec;
      }

      inline auto read(flat_buffer& buff) const {
         boost::system::error_code ec;
         stream->read(buff, ec);
         if (ec) {
            SILK_ERROR << "SHiP read failed : " << ec.message();
         }
         return ec;
      }

      template <typename F>
      inline void async_read(F&& func) const {
         auto buff = std::make_shared<flat_buffer>();

         stream->async_read(*buff, appbase::app().executor().get_priority_queue().wrap(80, 0,
            [buff, func](const auto ec, auto) {
               if (ec) {
                  SILK_ERROR << "SHiP read failed : " << ec.message();
               }
               func(ec, buff);
            })
         );
      }

      auto send_get_blocks_request(uint32_t start) {
         eosio::ship_protocol::request req = eosio::ship_protocol::get_blocks_request_v0{
            .start_block_num        = start,
            .end_block_num          = std::numeric_limits<uint32_t>::max(),
            .max_messages_in_flight = std::numeric_limits<uint32_t>::max(),
            .have_positions         = {},
            .irreversible_only      = false,
            .fetch_block            = true,
            .fetch_traces           = true,
            .fetch_deltas           = false
         };
         return send_request(req);
      }

      auto send_get_status_request() {
         eosio::ship_protocol::request req = eosio::ship_protocol::get_status_request_v0{};
         return send_request(req);
      }

      template <typename Buffer>
      eosio::ship_protocol::result get_result(Buffer&& b){
         auto data = b->data();
         eosio::input_stream bin = {(const char*)data.data(), (const char*)data.data() + data.size()};
         return eosio::from_bin<eosio::ship_protocol::result>(bin);
      }

      template <typename T>
      std::optional<native_block_t> to_native(T&& block) {

         if (!block.this_block) {
           //TODO: throw here?
           return std::nullopt;
         }

         native_block_t current_block = start_native_block(block);

         if (block.traces) {
            uint32_t num;
            eosio::varuint32_from_bin(num, *block.traces);
            SILK_DEBUG << "Block #" << block.this_block->block_num << " with " << num << " transactions";
            for (std::size_t i = 0; i < num; i++) {
               auto tt = eosio::from_bin<eosio::ship_protocol::transaction_trace>(*block.traces);
               const auto& trace = std::get<eosio::ship_protocol::transaction_trace_v0>(tt);
               if (trace.status != eosio::ship_protocol::transaction_status::executed) {
                  SILK_DEBUG << "Block #" << block.this_block->block_num << " ignore transaction with status " << (int)trace.status;
                  continue;
               }
               append_to_block(current_block, trace);
            }
         }

         current_block.lib = block.last_irreversible.block_num;
         return std::optional<native_block_t>(std::move(current_block));
      }

      template <typename BlockResult>
      inline native_block_t start_native_block(BlockResult&& res) const {
         native_block_t block;
         eosio::ship_protocol::signed_block sb;
         eosio::from_bin(sb, *res.block);

         block.block_num = res.this_block->block_num;
         block.id        = res.this_block->block_id;
         block.prev      = res.prev_block->block_id;
         block.timestamp = sb.timestamp.to_time_point().time_since_epoch().count();

         //SILK_INFO << "Started native block " << block.block_num;
         return block;
      }

      inline void append_to_block(native_block_t& block, const eosio::ship_protocol::transaction_trace_v0& trace) {
         channels::native_trx native_trx = {trace.id, trace.cpu_usage_us, trace.elapsed};
         const auto& actions = trace.action_traces;
         //SILK_DEBUG << "Appending transaction ";
         auto it = std::find_if(actions.begin(), actions.end(), [&](const auto& act) -> bool {
            return std::visit([&](const auto& a) { return a.receiver == core_account && a.act.name == evmtx_n; }, act);
         });
         auto action_to_search = it == actions.end() ? pushtx_n : evmtx_n;

         std::map<uint64_t, eosio::ship_protocol::action_trace> ordered_action_traces;
         for (std::size_t j = 0; j < actions.size(); ++j) {
            std::visit([&](auto& act) {
               if ((act.act.name == action_to_search || act.act.name == configchange_n) && core_account == act.receiver) {
                  if (!act.receipt.has_value()) {
                     SILK_ERROR << "action_trace does not have receipt";
                     throw std::runtime_error("action_trace does not have receipt");
                  }
                  uint64_t global_sequence = 0;
                  std::visit([&](auto &receipt) {
                     if (act.act.name == evmtx_n) {
                        uint32_t parent_act_index = act.creator_action_ordinal;
                        if (parent_act_index == 0) {
                           throw std::runtime_error("creator_action_ordinal can't be zero in evmtx");
                        }
                        parent_act_index--; // offset by 1
                        if (parent_act_index >= j) {
                           SILK_ERROR << "current action index:" << j << ", parent_act_index:" << parent_act_index;
                           throw std::runtime_error("parent action index must be less than current action index");
                        }
                        std::visit([&](auto& parent_act) {
                           if (!parent_act.receipt.has_value()) {
                              SILK_ERROR << "parent action does not have receipt";
                              throw std::runtime_error("parent action does not have receipt");
                           }
                           std::visit([&](auto &parent_act_receipt) {
                              global_sequence = parent_act_receipt.global_sequence;
                           }, parent_act.receipt.value());
                        }, actions[parent_act_index]);
                        SILK_DEBUG << "add evmtx sequence " << global_sequence 
                                   << ", parent action index " << parent_act_index;
                     } else if (act.act.name == configchange_n) {
                        global_sequence = 0;
                        SILK_DEBUG << "add configchange sequence " << global_sequence;
                     } else {
                        global_sequence = receipt.global_sequence;
                        SILK_DEBUG << "add pushtx sequence " << global_sequence;
                     }
                  }, act.receipt.value());
                  ordered_action_traces[global_sequence] = std::move(actions[j]);
               }
            }, actions[j]);
         }
         if (ordered_action_traces.size()) {   
            for (const auto &pair: ordered_action_traces) {
               std::visit([&](const auto& act) {
                  channels::native_action action = {
                     act.action_ordinal,
                     act.receiver,
                     act.act.account,
                     act.act.name,
                     std::vector<char>(act.act.data.pos, act.act.data.end)
                  };
                  
                  if (action.name == configchange_n) {
                     if (block.new_config.has_value()) {
                        SILK_ERROR << "multiple configchange in one block";
                        throw std::runtime_error("multiple configchange in one block");
                     }
                     if (native_trx.actions.size() || block.transactions.size()) {
                        SILK_ERROR << "configchange can only be the first action";
                        throw std::runtime_error("configchange can only be the first action");
                     }
                     block.new_config = action;
                  }
                  else {
                     if (block.new_config.has_value() && action.name == pushtx_n) {
                           SILK_ERROR << "pushtx and configchange found on the same transaction";
                           throw std::runtime_error("pushtx and configchange found on the same transaction");
                     }
                     if (native_trx.actions.size() && native_trx.actions.back().name != action.name) {
                           SILK_ERROR << "pushtx and evmtx found on the same transaction";
                           throw std::runtime_error("pushtx and evmtx found on the same transaction");
                     }
                     native_trx.actions.emplace_back(std::move(action));
                  }
               }, pair.second);
            }
            if(block.transactions.size() && block.transactions.back().actions.back().name != native_trx.actions.back().name) {
               SILK_ERROR << "pushtx and evmtx found on the same block";
               throw std::runtime_error("pushtx and evmtx found on the same block");
            }
            block.transactions.emplace_back(std::move(native_trx));
         }
      }
     
      auto get_status(auto& r){
         auto ec = send_get_status_request();
         if (ec) {
            return ec;
         }
         auto buff = std::make_shared<flat_buffer>();
         ec = read(*buff);
         if (ec) {
            return ec;
         }
         r = std::get<eosio::ship_protocol::get_status_result_v0>(get_result(buff));
         return ec;
      }

      void reset_connection() {
         // De facto entry point.
         if (stream) {
            // Try close connection gracefully but ignore return value.
            boost::system::error_code ec;
            stream->close(websocket::close_reason(websocket::close_code::normal),ec);

            // Determine if we should re-connect.
            if (++retry_count > max_retry) {
               // No more retry;
               sys::error("Max retry reached. No more reconnections.");
               return;
            }

            // Delay in the case of reconnection.
            std::this_thread::sleep_for(std::chrono::seconds(delay_second));
            SILK_INFO << "Trying to reconnect "<< retry_count << "/" << max_retry;
         }
         stream = std::make_shared<websocket::stream<tcp::socket>>(appbase::app().get_io_context());
         stream->binary(true);
         stream->read_message_max(0x1ull << 36);

         // CAUTION: we have to use async call here to avoid recursive reset_connection() calls.
         resolver->async_resolve( tcp::v4(), host, port, [this](const auto ec, auto res) {
            if (ec) {
               SILK_ERROR << "Resolver failed : " << ec.message();
               reset_connection();
               return;
            }

            // It should be fine to call connection and initial read synchronously as though they are 
            // blocking calls, it's only one thread and we have nothing more important to run anyway.
            auto ec2 = connect_stream(res);
            if (ec2) {
               reset_connection();
               return;
            }

            ec2 = initial_read();
            if (ec2) {
               reset_connection();
               return;
            }
            
            // Will call reset connection if necessary internally.
            sync();
         });         
      }

      void start_read() {
         async_read([this](const auto ec, auto buff) {
            if (ec) {
               SILK_INFO << "Trying to recover from SHiP read failure.";
               // Reconnect and restart sync.
               reset_connection();
               return;
            }
            auto block = to_native(std::get<eosio::ship_protocol::get_blocks_result_v0>(get_result(buff)));
            if(!block) {
               sys::error("Unable to generate native block");
               // No reset!
               return;
            }

            last_lib = block->lib;
            last_block_num = block->block_num;
            // reset retry_count upon successful read.
            retry_count = 0;

            native_blocks_channel.publish(80, std::make_shared<channels::native_block>(std::move(*block)));

            start_read();
         });
      }

      void sync() {
         SILK_INFO << "Start Syncing blocks.";
         // get available blocks range we can grab
         eosio::ship_protocol::get_status_result_v0 res = {};
         auto ec = get_status(res);
         if (ec) {
            reset_connection();
            return;
         }

         uint32_t start_from = 0;

         if (last_lib > 0) {
            // None zero last_lib means we are in the process of reconnection.
            // If last pushed block number is higher than LIB, we have the risk of fork and need to start from LIB.
            // Otherwise it means we are catching up blocks and can safely continue from next block.
            start_from = (last_lib > last_block_num ? last_block_num : last_lib) + 1;
            SILK_INFO << "Recover from disconnection, " << "last LIB is: " << last_lib
                     << ", last block num is: " << last_block_num
                     << ", continue from: " << start_from;
         }
         else {
            // Only take care of canonical header and input options when it's initial sync.
            if (start_from_canonical_height) {
               SILK_INFO << "Override head height with"
                        << "#" << *start_from_canonical_height;
            }

            auto start_block = appbase::app().get_plugin<engine_plugin>().get_canonical_block_at_height(start_from_canonical_height);
            if (!start_block) {
               sys::error("Unable to read canonical block");
               // No reset!
               return;
            }

            SILK_INFO << "Get_head_canonical_header: "
                     << "#" << start_block->header.number
                     << ", hash:" << silkworm::to_hex(start_block->header.hash())
                     << ", mixHash:" << silkworm::to_hex(start_block->header.prev_randao);

            start_from = utils::to_block_num(start_block->header.prev_randao.bytes) + 1;
            SILK_INFO << "Canonical header start from block: " << start_from;
            
         }
         
         if( res.trace_begin_block > start_from ) {
            SILK_ERROR << "Block #" << start_from << " not available in SHiP";
            sys::error("Start block not available in SHiP");
            // No reset!
            return;
         }

         SILK_INFO << "Starting from block #" << start_from;
         ec = send_get_blocks_request(start_from);
         if (ec) {
            reset_connection();
            return;
         }
         start_read();
      }

   std::optional<uint64_t> get_start_from_canonical_height() {
      return start_from_canonical_height;
   }

   private:
      std::shared_ptr<tcp::resolver>                  resolver;
      std::shared_ptr<websocket::stream<tcp::socket>> stream;
      constexpr static uint32_t                       priority = 40;
      std::string                                     host;
      std::string                                     port;
      abieos::abi                                     abi;
      channels::native_blocks::channel_type&          native_blocks_channel;
      eosio::name                                     core_account;
      uint32_t                                        last_lib;
      uint32_t                                        last_block_num;
      uint32_t                                        delay_second;
      uint32_t                                        max_retry;
      uint32_t                                        retry_count;
      std::optional<uint64_t>                         start_from_canonical_height;
};

ship_receiver_plugin::ship_receiver_plugin() : my(new ship_receiver_plugin_impl) {}
ship_receiver_plugin::~ship_receiver_plugin() = default;

void ship_receiver_plugin::set_program_options( appbase::options_description& cli, appbase::options_description& cfg ) {
   cfg.add_options()
      ("ship-endpoint", boost::program_options::value<std::string>()->default_value("127.0.0.1:8999"),
        "SHiP host address")
      ("ship-core-account", boost::program_options::value<std::string>()->default_value("evmevmevmevm"),
        "Account on the core blockchain that hosts the EVM Contract")
      ("ship-max-retry", boost::program_options::value<uint32_t>(),
        "Max retry times before give up when trying to reconnect to SHiP endpoints"  )
      ("ship-delay-second", boost::program_options::value<uint32_t>(),
        "Deply in seconds between each retry when trying to reconnect to SHiP endpoints"  )
      ("ship-start-from-canonical-height", boost::program_options::value<uint64_t>(),
        "Override evm canonical head block to start syncing from"  )
   ;
}

void ship_receiver_plugin::plugin_initialize( const appbase::variables_map& options ) {
   auto endpoint = options.at("ship-endpoint").as<std::string>();
   const auto& i = endpoint.find(":");
   auto core     = options.at("ship-core-account").as<std::string>();
   std::optional<uint64_t> start_from_canonical_height;
   uint32_t delay_second = 10;
   uint32_t max_retry = 0;

   if (options.contains("ship-start-from-canonical-height")) {
      start_from_canonical_height = options.at("ship-start-from-canonical-height").as<uint64_t>();
   }
   
   if (options.contains("ship-max-retry")) {
      max_retry = options.at("ship-max-retry").as<uint32_t>();
   }

   if (options.contains("ship-delay-second")) {
      delay_second = options.at("ship-delay-second").as<uint32_t>();
   }

   my->init(endpoint.substr(0, i), endpoint.substr(i+1), eosio::name(core), start_from_canonical_height, max_retry, delay_second);
   SILK_INFO << "Initialized SHiP Receiver Plugin";
}

void ship_receiver_plugin::plugin_startup() {
   SILK_INFO << "Started SHiP Receiver";
   my->reset_connection();

}

void ship_receiver_plugin::plugin_shutdown() {
   SILK_INFO << "Shutdown SHiP Receiver";
}

std::optional<uint64_t> ship_receiver_plugin::get_start_from_canonical_height() {
   return my->get_start_from_canonical_height();
}

