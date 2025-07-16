use std::sync::Arc;
use log::{info, warn, error, LevelFilter};
use anyhow::{Result, Context};
use crossbeam_channel::unbounded;
use tokio::runtime::Runtime;
use std::sync::atomic::{AtomicBool, Ordering};

// tray-icon specific imports
use tray_icon::{
    menu::{Menu, MenuItem, MenuEvent, PredefinedMenuItem}, // Changed CustomMenuItem to MenuItem
    TrayIconBuilder, TrayIconEvent,
    Icon, // Changed icon::Icon to Icon
};
use image::GenericImageView; // For loading icon data

// Tao for event loop
use tao::{
    event_loop::{ControlFlow, EventLoopBuilder},
    platform::macos::EventLoopExtMacOS, // For set_activation_policy
};

// Logging specific imports
use log4rs::append::console::ConsoleAppender;
use log4rs::append::file::FileAppender;
use log4rs::config::{Appender, Config, Root};
use log4rs::encode::pattern::PatternEncoder;

// MIDI Handler
use crate::midi_handler::MidiHandler;

// Declare the server module
mod server;
// Declare the MIDI handler module
mod midi_handler;

fn init_logging() -> Result<()> {
    // Pattern for log messages
    let log_pattern = "{d(%Y-%m-%d %H:%M:%S%.3f %Z)(utc)} [{l}] {M} - {m}{n}";
    // Log file path
    let log_file_path = "subpub_server.log";

    // Console appender
    let stdout = ConsoleAppender::builder()
        .encoder(Box::new(PatternEncoder::new(log_pattern)))
        .build();

    // File appender
    // TODO: Add log rotation in the future if needed
    let file_appender = FileAppender::builder()
        .encoder(Box::new(PatternEncoder::new(log_pattern)))
        .build(log_file_path)
        .context(format!("Failed to create file appender at {}", log_file_path))?;

    // Log4rs config
    let config = Config::builder()
        .appender(Appender::builder().build("stdout", Box::new(stdout)))
        .appender(Appender::builder().build("file", Box::new(file_appender)))
        .build(
            Root::builder()
                .appender("stdout")
                .appender("file")
                .build(LevelFilter::Debug), // Set global log level to Debug for more verbose output
        )
        .context("Failed to build log4rs config")?;

    // Initialize log4rs
    log4rs::init_config(config).context("Failed to initialize log4rs")?;

    Ok(())
}


fn main() -> Result<()> {
    // Initialize logging
    init_logging().context("Failed to initialize application logging")?;

    // Initialize MIDI Handler
    let midi_handler_arc = MidiHandler::new().context("Failed to initialize MIDI handler")?;
    info!("MIDI Handler creation attempted."); // MidiHandler::new() already logs its own success/failure

    info!("Starting SubPub Tray Icon Application with tray-icon...");

    // Icon
    let icon_path = "src/subpub.ico";
    let img = image::open(icon_path)
        .with_context(|| format!("Failed to load image from path: {}", icon_path))?;
    let (width, height) = img.dimensions();
    let mut rgba = img.to_rgba8().into_raw(); // Make rgba mutable

    // Invert RGB colors, keep Alpha
    // This will make dark colors light and light colors dark.
    // For a typical black icon on transparent, it should become white on transparent.
    info!("Inverting icon colors..."); // Log this one-time action
    for chunk in rgba.chunks_mut(4) { // Process 4 bytes at a time: R, G, B, A
        if chunk.len() == 4 {
            chunk[0] = 255 - chunk[0]; // Invert R
            chunk[1] = 255 - chunk[1]; // Invert G
            chunk[2] = 255 - chunk[2]; // Invert B
            // chunk[3] is Alpha, leave as is
        }
    }
    
    let icon = Icon::from_rgba(rgba, width, height)
        .with_context(|| format!("Failed to create icon from RGBA data from path: {}", icon_path))?;

    // Menu items
    const MENU_ITEM_START_ID: &str = "start_server";
    const MENU_ITEM_STOP_ID: &str = "stop_server";
    const MENU_ITEM_RELOAD_MIDI_ID: &str = "reload_midi_mappings"; // New ID
    const MENU_ITEM_QUIT_ID: &str = "quit_app";

    let tray_menu = Menu::new();
    // Corrected MenuItem::with_id arguments: id, text, enabled, accelerator
    let start_item = MenuItem::with_id(MENU_ITEM_START_ID, "Start Server", true, None);
    let stop_item = MenuItem::with_id(MENU_ITEM_STOP_ID, "Stop Server", true, None);
    let reload_midi_item = MenuItem::with_id(MENU_ITEM_RELOAD_MIDI_ID, "Reload MIDI Mappings", true, None); // New item
    let quit_item = MenuItem::with_id(MENU_ITEM_QUIT_ID, "Quit", true, None);
    
    tray_menu.append(&start_item).context("Failed to add 'Start Server' menu item")?;
    tray_menu.append(&stop_item).context("Failed to add 'Stop Server' menu item")?;
    tray_menu.append(&reload_midi_item).context("Failed to add 'Reload MIDI Mappings' menu item")?; // Add new item
    tray_menu.append(&PredefinedMenuItem::separator()).context("Failed to add separator")?;
    tray_menu.append(&quit_item).context("Failed to add 'Quit' menu item")?;

    // Channels for communication with server task
    let (server_shutdown_tx, server_shutdown_rx) = unbounded::<()>();
    // server_status_tx might be useful for updating menu item states (e.g., enable/disable Start/Stop)
    // For now, let's keep it simple.
    let (server_status_tx, _server_status_rx) = unbounded::<bool>(); 


    // Arc to hold the Tokio runtime handle and the server task handle
    let rt_handle_arc: Arc<std::sync::Mutex<Option<Runtime>>> = Arc::new(std::sync::Mutex::new(None));
    let server_task_handle_arc: Arc<std::sync::Mutex<Option<tokio::task::JoinHandle<Result<()>>>>> = Arc::new(std::sync::Mutex::new(None));
    
    let quit_flag = Arc::new(AtomicBool::new(false));
    
    // Create tao event loop & set activation policy for macOS
    let mut event_loop = EventLoopBuilder::<()>::with_user_event().build(); // Use with_user_event for custom events if needed later
    event_loop.set_activation_policy(tao::platform::macos::ActivationPolicy::Accessory);
    
    // The _tray_icon variable needs to be kept alive.
    // It's created here and its lifetime is tied to the main function's scope,
    // which is fine as event_loop.run will block.
    let _tray_icon_instance = TrayIconBuilder::new()
        .with_menu(Box::new(tray_menu)) // tray_menu was defined earlier
        .with_tooltip("SubPub Server")
        .with_icon(icon.clone()) // icon was defined earlier, clone if Icon is not Copy
        .build()
        .context("Failed to build tray icon")?;
    
    info!("Tray icon created. Starting tao event loop.");

    // Clone Arcs and other variables needed for the event loop closure
    let rt_handle_arc_clone = rt_handle_arc.clone();
    let server_task_handle_arc_clone = server_task_handle_arc.clone();
    let server_shutdown_tx_clone = server_shutdown_tx.clone(); // For stop and quit
    let server_status_tx_clone_for_start = server_status_tx.clone(); // Specifically for start
    let server_shutdown_rx_clone_for_start = server_shutdown_rx.clone(); // Specifically for start
    let quit_flag_clone_for_event_loop = quit_flag.clone();
    let midi_handler_clone_for_event_loop = midi_handler_arc.clone(); // Clone for event loop

    event_loop.run(move |_event, _, control_flow| {
        *control_flow = ControlFlow::Poll; 

        if quit_flag_clone_for_event_loop.load(Ordering::SeqCst) {
            info!("Quit flag set, exiting event loop via ControlFlow::Exit.");
            
            // Attempt to gracefully stop the server if it's running
            let mut rt_guard = rt_handle_arc_clone.lock().unwrap();
            let mut task_guard = server_task_handle_arc_clone.lock().unwrap();
            if let Some(rt) = rt_guard.take() {
                info!("Stopping server before exiting event loop...");
                if let Some(task_handle) = task_guard.take() {
                    server_shutdown_tx_clone.send(()).unwrap_or_else(|e| error!("Failed to send server shutdown signal on exit: {}",e));
                    rt.block_on(async {
                        if let Err(e) = task_handle.await {
                            error!("Server task failed to join during exit: {:?}", e);
                        }
                    });
                }
                rt.shutdown_background();
                info!("Server stopped during exit process.");
            }

            *control_flow = ControlFlow::Exit;
            return;
        }

        // Process menu events
        if let Ok(menu_event) = MenuEvent::receiver().try_recv() {
            // Removed verbose: info!("Menu event: {:?}", menu_event);
            match menu_event.id.0.as_str() {
                MENU_ITEM_START_ID => {
                    let mut rt_guard = rt_handle_arc_clone.lock().unwrap();
                    let mut task_guard = server_task_handle_arc_clone.lock().unwrap();

                    if rt_guard.is_none() {
                        info!("Attempting to start server via menu...");
                        let rt = Runtime::new().expect("Failed to create Tokio runtime");
                        let handle_for_spawn_call = rt.handle().clone();
                        let handle_for_async_block = rt.handle().clone();
                        *rt_guard = Some(rt); 

                        // Clone channels and MIDI handler needed for the new server task
                        let shutdown_rx_for_task = server_shutdown_rx_clone_for_start.clone();
                        let status_tx_for_task = server_status_tx_clone_for_start.clone();
                        let midi_handler_for_task = midi_handler_clone_for_event_loop.clone(); // Clone for server task

                        let task = handle_for_spawn_call.spawn(async move {
                            status_tx_for_task.send(true).unwrap_or_else(|e| error!("Failed to send server start status: {}",e));
                            // Pass midi_handler_for_task to run_server_application
                            // The signature of run_server_application will need to be updated
                            let result = server::run_server_application(
                                handle_for_async_block, 
                                shutdown_rx_for_task,
                                midi_handler_for_task // New argument
                            ).await;
                            status_tx_for_task.send(false).unwrap_or_else(|e| error!("Failed to send server stop status: {}",e));
                            result
                        });
                        *task_guard = Some(task);
                        info!("Server start initiated via menu.");
                    } else {
                        warn!("Server is already running (menu).");
                    }
                }
                MENU_ITEM_STOP_ID => {
                    let mut rt_guard = rt_handle_arc_clone.lock().unwrap();
                    let mut task_guard = server_task_handle_arc_clone.lock().unwrap();

                    if let Some(rt) = rt_guard.take() {
                        info!("Attempting to stop server via menu...");
                        if let Some(task_handle) = task_guard.take() {
                            server_shutdown_tx_clone.send(()).unwrap_or_else(|e| error!("Failed to send server shutdown signal: {}",e));
                            rt.block_on(async {
                                if let Err(e) = task_handle.await {
                                    error!("Server task failed to join: {:?}", e);
                                }
                            });
                        }
                        rt.shutdown_background();
                        info!("Server stop initiated via menu.");
                    } else {
                        warn!("Server is not running (menu).");
                    }
                }
                MENU_ITEM_QUIT_ID => {
                    info!("Quit menu item selected. Setting quit flag.");
                    quit_flag_clone_for_event_loop.store(true, Ordering::SeqCst);
                    // The actual server stop and exit will happen at the start of the next loop iteration.
                }
                MENU_ITEM_RELOAD_MIDI_ID => {
                    info!("Reload MIDI Mappings menu item selected.");
                    match midi_handler_clone_for_event_loop.lock() {
                        Ok(mut handler) => {
                            if let Err(e) = handler.reload_mappings() {
                                error!("Failed to reload MIDI mappings: {:?}", e);
                            } else {
                                info!("MIDI mappings reloaded successfully via menu.");
                            }
                        }
                        Err(e) => {
                            error!("Failed to lock MIDI handler for reloading mappings: {:?}", e);
                        }
                    }
                }
                _ => {
                    warn!("Unhandled menu event id: {:?}", menu_event.id);
                }
            }
        }

        // Process tray icon events (e.g., clicks on the icon itself)
        if let Ok(_tray_event) = TrayIconEvent::receiver().try_recv() { // Prefixed with _
            // Removed verbose: info!("Tray event: {:?}", _tray_event);
            // Handle tray icon clicks if needed, e.g.
            // match _tray_event.event {
            //    ClickType::Left => info!("Tray icon left clicked."),
            //    ClickType::Right => info!("Tray icon right clicked."),
            //    _ => {}
            // }
        }
        // No need for thread::sleep, ControlFlow::Poll handles waiting.
    });
    // event_loop.run is blocking and will typically not return.
    // The program exits when ControlFlow::Exit is set.
    // Thus, an Ok(()) here would be unreachable.
}
