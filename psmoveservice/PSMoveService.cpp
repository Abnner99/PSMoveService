//-- includes -----
//#define BOOST_ALL_DYN_LINK
#define BOOST_LIB_DIAGNOSTIC

#include "ServerNetworkManager.h"
#include "ServerRequestHandler.h"
#include "ControllerManager.h"
#include "ServerLog.h"

#include <boost/asio.hpp>
#include <boost/application.hpp>
#include <boost/program_options.hpp>
#include <fstream>
#include <cstdio>
#include <string>
#include <signal.h>

using namespace boost;

//-- constants -----
const int PSMOVE_SERVER_PORT = 9512;

//-- definitions -----
class PSMoveService
{
public:
    PSMoveService() 
        : m_io_service()
        , m_signals(m_io_service)
        , m_controller_manager()
        , m_request_handler(&m_controller_manager)
        , m_network_manager(&m_io_service, PSMOVE_SERVER_PORT, &m_request_handler)
        , m_status()
    {
        // Register to handle the signals that indicate when the server should exit.
        m_signals.add(SIGINT);
        m_signals.add(SIGTERM);
        #if defined(SIGQUIT)
        m_signals.add(SIGQUIT);
        #endif // defined(SIGQUIT)
        m_signals.async_wait(boost::bind(&PSMoveService::handle_termination_signal, this));
    }

    int operator()(application::context& context)
    {
        BOOST_APPLICATION_FEATURE_SELECT

        // Attempt to start and run the service
        try 
        {
            if (startup())
            {
                m_status = context.find<application::status>();

                while (m_status->state() != application::status::stoped)
                {
                    update();

                    boost::this_thread::sleep(boost::posix_time::milliseconds(1));
                }
            }
            else
            {
                std::cerr << "Failed to startup the PSMove service" << std::endl;
            }
        }
        catch (std::exception& e) 
        {
            std::cerr << e.what() << std::endl;
        }

        // Attempt to shutdown the service
        try 
        {
           shutdown();
        }
        catch (std::exception& e) 
        {
            std::cerr << e.what() << std::endl;
        }

        return 0;
    }

    bool stop(application::context& context)
    {
        return true;
    }

    bool pause(application::context& context)
    {
        return true;
    }

    bool resume(application::context& context)
    {
        return true;
    }

private:
    bool startup()
    {
        bool success= true;

        // Start listening for client connections
        if (success)
        {
            if (!m_network_manager.startup())
            {
                std::cerr << "Failed to initialize the service network manager" << std::endl;
                success= false;
            }
        }

        // Setup the request handler
        if (success)
        {
            if (!m_request_handler.startup())
            {
                std::cerr << "Failed to initialize the service request handler" << std::endl;
                success= false;
            }
        }

        // Setup the controller manager
        if (success)
        {
            if (!m_controller_manager.startup())
            {
                std::cerr << "Failed to initialize the controller manager" << std::endl;
                success= false;
            }
        }

        return success;
    }

    void update()
    {
        // Update the list of active tracked controllers
        // Send controller updates to the client
        m_controller_manager.update();

        // Process incoming/outgoing networking requests
        m_network_manager.update();
    }

    void shutdown()
    {
        // Disconnect any actively connected controllers
        m_controller_manager.shutdown();

        // Kill any pending request state
        m_request_handler.shutdown();

        // Close all active network connections
        m_network_manager.shutdown();
    }

    void handle_termination_signal()
    {
        std::cerr << "Received termination signal. Stopping Service." << std::endl;
        m_status->state(application::status::stoped);
    }

private:   
    // The io_service used to perform asynchronous operations.
    boost::asio::io_service m_io_service;

    // The signal_set is used to register for process termination notifications.
    boost::asio::signal_set m_signals;

    // Keep track of currently connected PSMove controllers
    ControllerManager m_controller_manager;

    // Generates responses from incoming requests sent to the network manager
    ServerRequestHandler m_request_handler;

    // Manages all TCP and UDP client connections
    ServerNetworkManager m_network_manager;

    // Whether the application should keep running or not
    std::shared_ptr<application::status> m_status;
};

//-- Entry program_optionsint ---
int main(int argc, char *argv[])
{
    // used to select between std:: and boost:: namespaces
    BOOST_APPLICATION_FEATURE_SELECT

    // Parse service options
    program_options::variables_map options_map;
    program_options::options_description desc;

    desc.add_options()
        ("help,h", "Shows help.")
        (",f", "Run as common application")
        ("log_level,l", program_options::value<std::string>(), "The level of logging to use: trace, debug, info, warning, error, fatal")
        ;

    try
    {
        program_options::store(program_options::parse_command_line(argc, argv, desc), options_map);
    }
    catch(boost::program_options::unknown_option &option)
    {
        std::cout << option.what() << std::endl;
        std::cout << "Valid Options: " << std::endl;
        std::cout << desc << std::endl;
        return 0;
    }

    if (options_map.count("-h"))
    {
        std::cout << "Valid Options: " << std::endl;
        std::cout << desc << std::endl;
        return 0;
    }

    // initialize logging system
    log_init(&options_map);

    SERVER_LOG_INFO("main") << "Starting PSMoveService";

    try
    {
        PSMoveService app;
        application::context app_context;

        // service aspects
        app_context.insert<application::path>(
            make_shared<application::path_default_behaviour>(argc, argv));

        app_context.insert<application::args>(
            make_shared<application::args>(argc, argv));

        // add termination handler
        application::handler<>::parameter_callback termination_callback
            = boost::bind<bool>(&PSMoveService::stop, &app, _1);

        app_context.insert<application::termination_handler>(
            make_shared<application::termination_handler_default_behaviour>(termination_callback));

        // To  "pause/resume" works, is required to add the 2 handlers.
#if defined(BOOST_WINDOWS_API) 
        // windows only : add pause handler     
        application::handler<>::parameter_callback pause_callback
            = boost::bind<bool>(&PSMoveService::pause, &app, _1);

        app_context.insert<application::pause_handler>(
            make_shared<application::pause_handler_default_behaviour>(pause_callback));

        // windows only : add resume handler
        application::handler<>::parameter_callback resume_callback
            = boost::bind<bool>(&PSMoveService::resume, &app, _1);

        app_context.insert<application::resume_handler>(
            make_shared<application::resume_handler_default_behaviour>(resume_callback));
#endif     

        // my common/server instantiation
        if (options_map.count("-d"))
        {
            return application::launch<application::server>(app, app_context);
        }
        else
        {
            return application::launch<application::common>(app, app_context);
        }
    }
    catch (boost::system::system_error& se)
    {
        SERVER_LOG_FATAL("main") << "Failed to start PSMoveService: " << se.what();
        return 1;
    }
    catch (std::exception &e)
    {
        SERVER_LOG_FATAL("main") << "Failed to start PSMoveService: " <<  e.what();
        return 1;
    }
    catch (...)
    {
        SERVER_LOG_FATAL("main") << "Failed to start PSMoveService: Unknown error.";
        return 1;
    }

    SERVER_LOG_INFO("main") << "Exiting PSMoveService";

    return 0;
}
