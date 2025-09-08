/*
 * (c) 2017-2020 Copyright, Real-Time Innovations, Inc. (RTI)
 * All rights reserved.
 *
 * RTI grants Licensee a license to use, modify, compile, and create derivative
 * works of the Software solely in combination with RTI Connext DDS. Licensee
 * may redistribute copies of the Software provided that all such copies are
 * subject to this License. The Software is provided "as is", with no warranty
 * of any type, including any warranty for fitness for any purpose. RTI is
 * under no obligation to maintain or support the Software. RTI shall not be
 * liable for any incidental or consequential damages arising out of the use or
 * inability to use the Software. For purposes of clarity, nothing in this
 * License prevents Licensee from using alternate versions of DDS, provided
 * that Licensee may not combine or link such alternate versions of DDS with
 * the Software.
 */

#include "config/XmlEntities.hpp"
#include "config/XmlTransformationParams.hpp"
#include "plugins/adapters/DdsOpcUaAdapterProperty.hpp"
#include "plugins/adapters/OpcUaAttributeServiceStreamWriter.hpp"
#include "plugins/adapters/OpcUaConnection.hpp"

namespace rti { namespace ddsopcua { namespace adapters {
    
OpcUaConnection::OpcUaConnection(
        const DdsOpcUaAdapterProperty& adapter_property,
        const rti::routing::PropertySet& connection_property)
        : adapter_property_(adapter_property),
          connection_property_(connection_property),
          opcua_attributeservice_streamreader_(nullptr),
          opcua_client_()
{
    // Fully-qualified name of the opcua server property associated with the
    // connection
    std::string opcua_server_xml_fqn = connection_property.at(
            config::XmlTransformationParams ::
                    DDSOPCUA_OPCUA_CONNECTION_FQN_PROPERTY);

    // Get the server URL from the connection property
    std::string server_uri = connection_property_.at(
            config::XmlTransformationParams ::
                    DDSOPCUA_OPCUA_CONNECTION_SERVER_URL_PROPERTY);

    // Configure new OPC UA Client
    opcua::sdk::client::ClientProperty client_property;
    config::XmlOpcUaClient::get_client_property(
            client_property,
            adapter_property_.xml_root(),
            opcua_server_xml_fqn);
    opcua_client_.reset(client_property);
    run_async_timeout_ = client_property.run_async_timeout;

    // Configure the reconnect information
    reconnect_cfg_.server_uri = server_uri;
    reconnect_cfg_.max_attempts =
                client_property.local_connection_reconnect_max_attempts;
    reconnect_cfg_.reconnect_interval =
            client_property.local_connection_reconnect_interval;

    // Connect to new OPC UA Client    
    opcua_client_.connect(server_uri);
    opcua_client_connected_ = true;
    opcua_client_async_thread_ = std::thread(
            run_opcua_client,
            std::ref(opcua_client_),
            std::ref(reconnect_cfg_),
            std::ref(managed_subscribers_),
            std::ref(adapter_property_.shutdown_hook()),
            std::ref(opcua_client_connected_),
            run_async_timeout_);
}

OpcUaConnection::~OpcUaConnection()
{
    opcua_client_.disconnect();
    opcua_client_connected_ = false;
    if (opcua_client_async_thread_.joinable()) {
        opcua_client_async_thread_.join();
    }
}

rti::routing::adapter::StreamWriter* OpcUaConnection::create_stream_writer(
        rti::routing::adapter::Session* session,
        const rti::routing::StreamInfo& stream_info,
        const rti::routing::PropertySet& property)
{
    rti::routing::adapter::StreamWriter* stream_writer = nullptr;

    if (stream_info.stream_name() == "OpcUaAttributeServiceSet_Request") {
        stream_writer = new OpcUaAttributeServiceStreamWriter(
                opcua_client_,
                opcua_attributeservice_streamreader_);
    }

    return stream_writer;
}

void OpcUaConnection::delete_stream_writer(
        rti::routing::adapter::StreamWriter* stream_writer)
{
    delete stream_writer;
}

rti::routing::adapter::StreamReader* OpcUaConnection::create_stream_reader(
        rti::routing::adapter::Session*,
        const rti::routing::StreamInfo& stream_info,
        const rti::routing::PropertySet& stream_reader_property,
        rti::routing::adapter::StreamReaderListener* listener)
{
    rti::routing::adapter::StreamReader* stream_reader = nullptr;

    if (stream_info.stream_name() == "OpcUaAttributeServiceSet_Reply") {
        stream_reader = opcua_attributeservice_streamreader_ =
                new OpcUaAttributeServiceStreamReader(
                        stream_info,
                        opcua_client_);
    } else {
        auto opcua_subs_sr =
                std::shared_ptr<OpcUaSubscriptionStreamReader>(nullptr);
        try {
            opcua_subs_sr = std::make_shared<OpcUaSubscriptionStreamReader>(
                    adapter_property_,
                    stream_info,
                    stream_reader_property,
                    listener,
                    opcua_client_);
            opcua_subs_sr->initialize_subscription();
            managed_subscribers_.push_back(opcua_subs_sr);
        } catch (const std::exception& e) {
            GATEWAYLog_exception(&DDSOPCUA_LOG_ANY_s, e.what());
            if (opcua_subs_sr != nullptr) {
                opcua_subs_sr->finalize_subscription();
            }
            return nullptr;
        }

        stream_reader = opcua_subs_sr.get();
    }    

    return stream_reader;
}

void OpcUaConnection::delete_stream_reader(
        rti::routing::adapter::StreamReader *stream_reader)
{
    if (stream_reader != nullptr) {
        delete stream_reader;

        auto it = std::remove_if(
                managed_subscribers_.begin(),
                managed_subscribers_.end(),
                [stream_reader](
                        const std::shared_ptr<OpcUaSubscriptionStreamReader>&
                                sp) { return sp.get() == stream_reader; });
    }
}

rti::opcua::sdk::client::Client& OpcUaConnection::connection_client()
{
    return opcua_client_;
}

void OpcUaConnection::run_opcua_client(
        opcua::sdk::client::Client& opcua_client,
        rti::ddsopcua::utils::ReconnectConfig& config,
        streamreadervector_t& managed_subscribers,
        rti::ddsopcua::utils::ServiceShutdownHook& shutdown_hook,
        bool& client_connected,
        const uint16_t timeout)
{
    bool should_exit = false, disconnected = false;
    int attempt_count = 0, last_attempt_time = 0;
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    while (!should_exit) {
        if (client_connected && !disconnected) {
            status = opcua_client.run_iterate(timeout);
            if (UA_STATUSCODE_GOOD == status) {
                // Reset attempt count on successful operation
                attempt_count = 0;
                last_attempt_time = 0;
            } else {
                GATEWAYLog_local(
                        &DDSOPCUA_LOG_ANY_ss,
                        "Client operation failed: ",
                        UA_StatusCode_name(status));

                disconnected = true;
                client_connected = false;
            }
        } else if (
                disconnected
                && config.max_attempts > 0) {
            // Check if we've exceeded max attempts
            if (attempt_count >= config.max_attempts) {
                GATEWAYLog_local(
                        &DDSOPCUA_LOG_ANY_s,
                        "Max attempts reached. Abandoning reconnection.");
                should_exit = true;
                continue;
            }

            time_t current_time = time(nullptr);
            int elapsed_ms =
                    (current_time - last_attempt_time) * 1000;
            if (last_attempt_time == 0
                || elapsed_ms >= config.reconnect_interval) {
                ++attempt_count;
                last_attempt_time = current_time;

                GATEWAYLog_local(
                        &DDSOPCUA_LOG_ANY_ss,
                        "Reconnection attempt ",
                        std::string(
                                std::to_string(attempt_count)
                                + '/'
                                + std::to_string(config.max_attempts))
                                .c_str());
                try {
                    opcua_client.connect(config.server_uri);

                    // rebuild the subscriptions for the new connection.
                    std::for_each(
                            managed_subscribers.begin(),
                            managed_subscribers.end(),
                            [](const std::shared_ptr<
                                    OpcUaSubscriptionStreamReader>& reader) {
                                if (reader) {
                                    reader->finalize_subscription();
                                    reader->initialize_subscription();
                                }
                            });

                    GATEWAYLog_local(
                            &DDSOPCUA_LOG_ANY_s,
                            "Reconnection successful!");
                    disconnected = false;
                    client_connected = true;
                    // Don't reset attempt_count here - wait for successful
                    // run_iterate
                } catch (const rti::ddsopcua::GatewayException& e) {
                    UA_StatusCode status =
                            static_cast<UA_StatusCode>(e.error_code());
                    GATEWAYLog_local(
                            &DDSOPCUA_LOG_ANY_ss,
                            "Reconnection failed: ",
                            UA_StatusCode_name(status));

                    if (attempt_count
                        >= config.max_attempts) {
                        GATEWAYLog_local(
                                &DDSOPCUA_LOG_ANY_s,
                                "Max attempts reached. Abandoning "
                                "reconnection.");
                        should_exit = true;
                    }
                }
            } else {
                // Wait before next attempt
                int elapsed_ms =
                        (current_time - last_attempt_time)
                        * 1000;
                int wait_ms = config.reconnect_interval - elapsed_ms;

                GATEWAYLog_local(
                        &DDSOPCUA_LOG_ANY_s,
                        std::string(
                                "Waiting " + std::to_string(wait_ms)
                                + " ms before next reconnection attempt")
                                .c_str());

                std::this_thread::sleep_for(
                        std::chrono::milliseconds(std::min(wait_ms, 1000)));
            }
        } else if (
                disconnected
                && config.max_attempts == 0) {
            // Reconnection disabled (max_attempts = 0)
            GATEWAYLog_local(
                    &DDSOPCUA_LOG_ANY_s,
                    "Disconnected and reconnection disabled. Exiting.");

            should_exit = true;
        } else {
            // shutting down or a state we can't handle
            should_exit = true;
        }
    }

    shutdown_hook.shutdown_service();
}

}}}  // namespace rti::ddsopcua::adapters
