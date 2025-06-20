/**
 * @file IrisRestfulNetworking.cpp
 * @author Ryan Landvater (ryanlandvater [at] gmail [dot] com)
 * @brief Based upon the work of Vinnie Falco and Boost/Beast
 * @version 0.1
 * @date 2025-06-07
 * 
 * @copyright Copyright (c) 2025 Iris Developers
 * 
 */

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif // __clang__
#include <boost/beast.hpp>          // Beast websocket protocol
#include <boost/asio/strand.hpp>    // Asio strand-style execution
#include <boost/exception/all.hpp>  // Boost runtime exceptions
#ifdef __clang__
#pragma clang diagnostic pop
#endif // __clang__

#define BOOST_IMPLEMENT // Allow for class definitions 
namespace   net       = boost::asio;
namespace   beast     = boost::beast;
namespace   ip        = net::ip;
using       tcp       = ip::tcp;
namespace   http      = beast::http;
#include "IrisRestfulPriv.hpp"

namespace Iris {
namespace RESTful {
__INTERNAL__Session::__INTERNAL__Session(ASIOSocket_t&& socket) :
stream(std::make_unique<ASIOStream_t>(std::move(socket)))
{
    std::cout <<"NEW SESSION\n";
}
__INTERNAL__Session::~__INTERNAL__Session() {
    std::cout <<"SESSION EXPIRED\n";
}
__INTERNAL__Networking::__INTERNAL__Networking(const ServerCallbacks& callbacks) :
_callbacks  (callbacks),
_reactors   (IRIS_CONCURRENCY),
_context    (std::make_shared<ASIOContext_t>(_reactors.size())),
_guard      (std::make_shared<ASIOGuard_t>(_context->get_executor())),
_acceptor   (nullptr),
ACTIVE      (true)
{
    for (auto&& thread : const_cast<Threads&>(_reactors))
        thread = std::thread {[this](){
            // Run the context run loop within
            // a controlled try catch enviornment
            // for runtime exception recovery
            while (ACTIVE) try {
                _context->run();
            } catch (std::runtime_error& error) {
                std::cout   << "Network socket error: "
                            << error.what() << "\n";
            } catch (beast::error_code& error) {
                std::cout   << "Network socket error: "
                            << error.message() << "\n";
            } catch (...) {
                std::cout   << "Undefined network error\n";
            }
        }};
}
__INTERNAL__Networking::~__INTERNAL__Networking ()
{
    // Inactivate the context loops
    ACTIVE = false;
    
    // Interrupt the oustanding acceptor call
    _acceptor->cancel();
    
    // remove the execution guard
    const_cast<ASIOGuard&>(_guard) = nullptr;
    
    // Pump the event loop to unblock the reactor threads
    _context->poll();
    
    // And wait for all reactor threads to exit.
    for (auto&& thread : const_cast<Threads&>(_reactors)) {
        if (thread.joinable())
            thread.join();
    }
}
void __INTERNAL__Networking::listen(uint16_t port){
    if (_acceptor && _acceptor->is_open()) throw std::runtime_error
        ("networking acceptor already active");
    
    beast::error_code error;
    // NOTE: If set to IPv6, IPv4 connections will fire 2 acceptions (once for downgraded protocol)
    tcp::endpoint endpoint = tcp::endpoint(ip::tcp::v4(), port);
    
    // Create a new acceptor and give it a separate strand. It will pass the strand to the new socket.
    _acceptor = std::make_shared<ASIOAcceptor_t>(net::make_strand(_context->get_executor()));
    if (!_acceptor) throw std::runtime_error
        ("failed to create acceptor");
   
    // Open the acceptor
    _acceptor->open(endpoint.protocol(), error);
    if (error) throw std::runtime_error
        ("Failed to open acceptor: " + error.message() +
          "[FILE " + __FILE__ + "; LINE " + std::to_string(__LINE__) + "]");

    // Allow socket to be bound to a used address (maybe don't need to)
    _acceptor->set_option(net::socket_base::reuse_address(true), error);
    if (error) throw std::runtime_error
        ("Failed to set acceptor to reuse address option: " + error.message());

    // Bind to the server address
    _acceptor->bind(endpoint, error);
    if (error) throw std::runtime_error
        ("Failed to bind endpoint to acceptor: " + error.message() +
         "[FILE " + __FILE__ + "; LINE " + std::to_string(__LINE__) + "]");

    // Start listening for connections
    _acceptor->listen(net::socket_base::max_listen_connections, error);
    if (error) throw std::runtime_error
        ("Failed to listen with acceptor: " + error.message() +
         "[FILE " + __FILE__ + "; LINE " + std::to_string(__LINE__) + "]");

    accept_connection(_acceptor);
}
void __INTERNAL__Networking::accept_connection(const ASIOAcceptor &acceptor)
{
    // Accept incoming connections
    // Note: If the sever is set to IPv6, this will fire twice upon a IPv4 request
    // Create a new strand; each acceptance carries the strand with the generated socket
    acceptor->async_accept(net::make_strand(acceptor->get_executor()),[this, acceptor]
                           (beast::error_code error, ASIOSocket_t socket){
        
        // Do not log an aborted operation; it means we are shutting the server down.
        if (error == net::error::operation_aborted) return;
        if (error) throw std::runtime_error
            ("Failed to accept an incomming connection: " + error.message());
        
        // Perpetuate the accept connection calls to keep the acceptor alive
        // If we have closed the acceptor, then gracefully exit and destroy acceptor
        if (acceptor->is_open()) accept_connection (acceptor);
        
        // Create a stream and begin reading messages
        auto session = std::make_shared<__INTERNAL__Session>(std::move(socket));
        read_request (session);
    });
}
void __INTERNAL__Networking::read_request(const Session &session)
{
    // Extend the expiration time
    // This is ABSOLUTELY VITAL. Failure to do this will signficantly affect performance
    session->stream->expires_after(std::chrono::seconds(30));
    
    auto buffer  = std::make_shared<ASIOBuffer_t>();
    auto parser  = std::make_shared<http::request_parser<http::string_body>>();
    // This is a light-weight server; we don't expect big requests
    parser->header_limit(1024);
    parser->body_limit(2048);
    
//    http::async_read(*(session->stream), *buffer, *request,[this, session, buffer, request]
    http::async_read(*(session->stream), *buffer, *parser,[this, session, buffer, parser]
                     (beast::error_code error, size_t bytes_transferred) {
        if (error) {
            if(error == http::error::end_of_stream ||
               error == beast::error::timeout)
                return close_stream (session);
            
            const auto response = std::make_shared<HTTPResponse_t>();
            response->version(11);
            response->set(http::field::content_type, "text/plain");
            response->keep_alive(parser->is_header_done()?parser->keep_alive():false);
            response->set(http::field::server, "IrisRESTful");
            if (error == http::error::header_limit) {
                // PROTECTION FROM DOS ATTACKS
                response->result(http::status::request_header_fields_too_large);
                response->body() = "IrisRESTful API HTTP header-length limit (1024) bytes exceeded";
                send_response (session, response);
            } else if (error == http::error::body_limit) {
                response->result(http::status::payload_too_large);
                response->body() = "IrisRESTful API payload-length limit (2048) bytes exceeded";
                send_response (session, response);
            } else {
                response->result(http::status::unknown);
                response->body() = "IrisRESTful API encountered undefined error: " + error.message();
                send_response(session, response);
            }
            return;
        }
        
        // Begin interpreting the request
        interpret_request(session, HTTPRequest_t(parser->release()));
    });
}
// Return a reasonable mime type based on the extension of a file.
inline beast::string_view
mime_type(beast::string_view path)
{
    using beast::iequals;
    auto const ext = [&path]
    {
        auto const pos = path.rfind(".");
        if(pos == beast::string_view::npos)
            return beast::string_view{};
        return path.substr(pos);
    }();
    if(iequals(ext, ".htm"))  return "text/html";
    if(iequals(ext, ".html")) return "text/html";
    if(iequals(ext, ".php"))  return "text/html";
    if(iequals(ext, ".css"))  return "text/css";
    if(iequals(ext, ".txt"))  return "text/plain";
    if(iequals(ext, ".js"))   return "application/javascript";
    if(iequals(ext, ".json")) return "application/json";
    if(iequals(ext, ".xml"))  return "application/xml";
    if(iequals(ext, ".swf"))  return "application/x-shockwave-flash";
    if(iequals(ext, ".flv"))  return "video/x-flv";
    if(iequals(ext, ".png"))  return "image/png";
    if(iequals(ext, ".jpe"))  return "image/jpeg";
    if(iequals(ext, ".jpeg")) return "image/jpeg";
    if(iequals(ext, ".jpg"))  return "image/jpeg";
    if(iequals(ext, ".gif"))  return "image/gif";
    if(iequals(ext, ".bmp"))  return "image/bmp";
    if(iequals(ext, ".ico"))  return "image/vnd.microsoft.icon";
    if(iequals(ext, ".tiff")) return "image/tiff";
    if(iequals(ext, ".tif"))  return "image/tiff";
    if(iequals(ext, ".svg"))  return "image/svg+xml";
    if(iequals(ext, ".svgz")) return "image/svg+xml";
    return "application/text";
}
inline HTTPResponse GENERATE_STRING_GET_RESPONSE (const GetResponse &response) {
    HTTPResponse msg = std::make_shared<HTTPResponse_t>();
    switch (response.type) {
        case GetResponse::GET_RESPONSE_UNDEFINED:
        case GetResponse::GET_RESPONSE_MALFORMED_REQ:
            msg->result(http::status::bad_request);
            msg->set(http::field::content_type, "application/text");
            break;
        case GetResponse::GET_RESPONSE_FILE_NOT_FOUND:
            msg->result(http::status::not_found);
            msg->set(http::field::content_type, "application/text");
            break;
        case GetResponse::GET_RESPONSE_METADATA:
            msg->result(http::status::ok);
            msg->set(http::field::content_type, "application/json");
            break;
        case GetResponse::GET_RESPONSE_TILE:
            assert(false && "ERROR: cannot generate http::string_body for GET_RESPONSE_TILE response; this is a binary response");
            throw std::runtime_error ("ERROR: cannot generate http::string_body for GET_RESPONSE_TILE response; this is a binary response");
    }
    msg->version(11);
    msg->set(http::field::server, "IrisRESTful");
    msg->keep_alive(response.keep_alive);
    msg->body() = serialize_get_response(response);
    msg->prepare_payload();
    return msg;
}
void __INTERNAL__Networking::interpret_request(const Session& session, const HTTPRequest_t &request)
{
    bool keep_alive = request.keep_alive();
    
    // TODO: Should we add file support here (JS/HTML etc...)

    // The _callbacks.on_X_Request callbacks use a nested callback for a VERY good reason.
    // This allows the __INTERNAL__Server instance to push the implementation off the stack
    // to a separate worker thread and remove it from this ASIO/NetowrkingTS reactor thread.
    // I want to allow the ASIO threads to dedicate all clock cycles to clearing the network queue.
    // Do not fuck with this design if you don't know what I'm talking about or without asking me.
    // - Ryan
    switch (request.method()) {
        case boost::beast::http::verb::head:
        case boost::beast::http::verb::get:
            _callbacks.onGetRequest(session, std::string(request.target()),[this,session, keep_alive]
                                    (const std::unique_ptr<GetResponse>& response){
                response->keep_alive = keep_alive;
                
                switch (response->type) {
                    case GetResponse::GET_RESPONSE_UNDEFINED:
                    case GetResponse::GET_RESPONSE_MALFORMED_REQ:
                    case GetResponse::GET_RESPONSE_FILE_NOT_FOUND:
                    case GetResponse::GET_RESPONSE_METADATA:
                        return send_response(session, GENERATE_STRING_GET_RESPONSE(*response));
                    case GetResponse::GET_RESPONSE_TILE:
                    {
                        HTTPResponseBuffer msg  = std::make_shared<HTTPResponseBuffer_t>();
                        auto tile_response      = reinterpret_cast<GetTileResponse*>(response.get());
                        msg->version(11);
                        msg->result(http::status::ok);
                        msg->set(http::field::server, "IrisRESTful");
                        msg->set(http::field::content_type, "image/jpeg");
//                        msg->content_length(tile_response->pixelData->size());
                        msg->body().data        = tile_response->pixelData->data();
                        msg->body().size        = tile_response->pixelData->size();
                        msg->body().more        = false;
                        msg->keep_alive(keep_alive);
                        msg->prepare_payload();
//                        msg->prepare_payload();
                        send_buffer(session, msg, tile_response->pixelData);
                        return;
                    }
                }
            }); return;
        case http::verb::post:
        case http::verb::put:
        case http::verb::patch:
        case http::verb::delete_:
        case http::verb::options:
        default: {
            
        } return;
        case http::verb::connect: {
            
        }
    }
}
void __INTERNAL__Networking::send_response(const Session &session, const HTTPResponse &response)
{
    http::async_write(*(session->stream), *response, [this, session, response]
                      (beast::error_code error, size_t bytes_transferred) {
        if (error) std::cout    << "Error writing response to stream: "
                                << error.message();
        if (response->keep_alive() && session->stream->socket().is_open())
                read_request (session);
        else    close_stream (session);
    });
}
void __INTERNAL__Networking::send_buffer(const Session &session, const HTTPResponseBuffer &response, const Buffer &buffer)
{
    auto serializer = std::make_shared<http::serializer<false, http::buffer_body>> (*response);
    
//    write_some(buffer, response, serializer, session);
    http::async_write(*(session->stream), *response, [this, session, response, buffer]
                      (beast::error_code error, size_t bytes_transferred) {
        if (error) std::cout    << "Error writing response to stream: "
                                << error.message();
        if (response->keep_alive() && session->stream->socket().is_open())
                read_request (session);
        else    close_stream (session);
    });
}
void __INTERNAL__Networking::close_stream(const Session &session)
{
    beast::error_code error;
    session->stream->socket().shutdown(tcp::socket::shutdown_send, error);
}
} // END RESTFUL
} // END IRIS
