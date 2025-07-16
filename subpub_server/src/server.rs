use std::collections::HashSet;
use std::net::SocketAddr;
use std::sync::{Arc, Mutex}; // Added Mutex
use tokio::net::UdpSocket;
use tokio::time::{sleep, Duration}; // For NoteOnOff delay
use log::{info, warn, error, debug}; // Added debug
use serde::Deserialize;
use crate::midi_handler::{MidiHandler, MidiAction, MidiActionType}; // Added Handler and related types
use dashmap::DashMap;
use std::net::{IpAddr, Ipv4Addr};
use anyhow::{Result, Context}; // Ensure Context is imported
use crossbeam_channel::Receiver;
use tokio::runtime::Handle;

// Constants
pub const BIND_ADDRESS: &str = "127.0.0.1:7878";
pub const MULTICAST_ADDRESS: &str = "192.168.0.100:50100";
pub const DISCOVERY_MESSAGE: &str = "DISCOVER_SUBPUB_SERVER";
pub const DISCOVERY_RESPONSE_PREFIX: &str = "SUBPUB_SERVER_AT:";

// Type alias
pub type Subscribers = Arc<DashMap<String, HashSet<SocketAddr>>>;

// Server processing loop
pub async fn run_server_processing_loop(
    socket: Arc<UdpSocket>,
    subscribers: Subscribers,
    midi_handler_arc: Arc<Mutex<MidiHandler>>,
    runtime_handle: Handle, // Added for spawning NoteOnOff delay tasks
) -> Result<(), Box<dyn std::error::Error + Send + Sync + 'static>> {
    let mut buf = [0; 1024];

    loop {
        let (len, addr) = socket.recv_from(&mut buf).await?;
        debug!("Processing message: {} bytes from {}", len, addr);
        let message_str = match std::str::from_utf8(&buf[..len]) {
            Ok(s) => s.trim(),
            Err(e) => {
                error!("Received non-UTF8 data from {}: {}", addr, e);
                continue;
            }
        };

        info!("Received from {}: {}", addr, message_str);

        let parts: Vec<&str> = message_str.splitn(3, ':').collect();

        if parts.len() < 2 {
            warn!("Invalid message format from {}: {}", addr, message_str);
            continue;
        }

        let action = parts[0].to_uppercase();
        let channel_name = parts[1].to_string();
        let payload = if parts.len() == 3 { Some(parts[2]) } else { None };

        match action.as_str() {
            "SUB" => {
                info!("Client {} subscribed to channel '{}'", addr, channel_name);
                subscribers.entry(channel_name.clone()).or_default().value_mut().insert(addr);
            }
            "UNSUB" => {
                info!("Client {} unsubscribed from channel '{}'", addr, channel_name);
                let mut channel_was_emptied = false;
                if let Some(mut channel_set_ref) = subscribers.get_mut(&channel_name) {
                    let removed = channel_set_ref.value_mut().remove(&addr);
                    if removed && channel_set_ref.value().is_empty() {
                        channel_was_emptied = true;
                    }
                }
                if channel_was_emptied {
                    subscribers.remove(&channel_name);
                    info!("Channel '{}' is now empty and removed.", channel_name);
                }
            }
            "PUB" => {
                if let Some(p) = payload {
                    info!("Client {} published to channel '{}': {}", addr, channel_name, p);
                    
                    // MIDI Processing
                    process_midi_actions(&channel_name, p, &midi_handler_arc, &runtime_handle).await;

                    // Existing PubSub forwarding
                    let mut subs_to_notify: Vec<SocketAddr> = Vec::new();
                    if let Some(channel_set_ref) = subscribers.get(&channel_name) {
                        subs_to_notify = channel_set_ref.value().iter().cloned().collect();
                    }

                    if !subs_to_notify.is_empty() {
                        for subscriber_addr in subs_to_notify {
                            // info!("Forwarding message to subscriber {} on channel '{}'", subscriber_addr, channel_name); // Can be verbose
                            if let Err(e) = socket.send_to(p.as_bytes(), subscriber_addr).await {
                                error!("Failed to send pubsub message to {}: {}", subscriber_addr, e);
                            }
                        }
                    } else {
                        // info!("No subscribers for channel '{}'. Message not forwarded.", channel_name); // Can be verbose
                    }
                } else {
                    warn!("PUB action from {} to channel '{}' without payload.", addr, channel_name);
                }
            }
            _ => {
                warn!("Unknown action '{}' from {}: {}", action, addr, message_str);
            }
        }
    }
}

// Represents the optional fields that can be sent in a JSON payload to override the base mapping.
#[derive(Deserialize, Debug, Default)]
struct PayloadOverride {
    action_type: Option<MidiActionType>,
    ch: Option<u8>,
    note: Option<u8>,
    vel: Option<u8>,
    dur: Option<u64>,
    control_num: Option<u8>,
    value: Option<u8>,
}

async fn process_midi_actions(
    topic: &str,
    payload_str: &str,
    midi_handler_arc: &Arc<Mutex<MidiHandler>>,
    runtime_handle: &Handle,
) {
    let mut handler = midi_handler_arc.lock().unwrap();
    
    // 1. Get the base actions from the mapping file for the current topic.
    if let Some(base_actions) = handler.get_actions_for_topic(topic) {
        debug!("Found {} base actions for topic '{}'", base_actions.len(), topic);

        // 2. Parse the payload for any overrides.
        // If parsing fails, overrides remain Default::default() (all None),
        // so the base action is used as-is. This handles the "simple ping" case.
        let overrides: PayloadOverride = serde_json::from_str(payload_str).unwrap_or_default();

        for base_action in base_actions {
            // 3. Merge the base action with any overrides from the payload.
            let final_action = MidiAction {
                action_type: overrides.action_type.clone().unwrap_or(base_action.action_type),
                channel: overrides.ch.unwrap_or(base_action.channel),
                note: overrides.note.or(base_action.note),
                velocity: overrides.vel.or(base_action.velocity),
                duration_ms: overrides.dur.or(base_action.duration_ms),
                control_num: overrides.control_num.or(base_action.control_num),
                value: overrides.value.or(base_action.value),
            };

            // 4. Construct and send the final MIDI message.
            let midi_msg: Option<Vec<u8>> = match final_action.action_type {
                MidiActionType::NoteOn => Some(vec![
                    0x90 + (final_action.channel & 0x0F),
                    final_action.note.unwrap_or(60),
                    final_action.velocity.unwrap_or(127).clamp(0, 127),
                ]),
                MidiActionType::NoteOff => Some(vec![
                    0x80 + (final_action.channel & 0x0F),
                    final_action.note.unwrap_or(60),
                    final_action.velocity.unwrap_or(0).clamp(0, 127),
                ]),
                MidiActionType::NoteOnOff => {
                    let note = final_action.note.unwrap_or(60);
                    let vel = final_action.velocity.unwrap_or(127);
                    let dur = final_action.duration_ms.unwrap_or(50);
                    let note_on_msg = vec![0x90 + (final_action.channel & 0x0F), note, vel.clamp(0, 127)];
                    let note_off_msg = vec![0x80 + (final_action.channel & 0x0F), note, 0];

                    if let Err(e) = handler.send_midi_message(&note_on_msg) {
                        error!("Failed to send merged MIDI NoteOn for {}: {:?}", topic, e);
                    }
                    
                    let midi_handler_clone = Arc::clone(midi_handler_arc);
                    let topic_clone = topic.to_string();
                    runtime_handle.spawn(async move {
                        sleep(Duration::from_millis(dur)).await;
                        let mut handler_clone = midi_handler_clone.lock().unwrap();
                        if let Err(e) = handler_clone.send_midi_message(&note_off_msg) {
                            error!("Failed to send merged delayed MIDI NoteOff for {}: {:?}", topic_clone, e);
                        }
                    });
                    None // Handled internally
                }
                MidiActionType::Cc => Some(vec![
                    0xB0 + (final_action.channel & 0x0F),
                    final_action.control_num.unwrap_or(0),
                    final_action.value.unwrap_or(0).clamp(0, 127),
                ]),
                MidiActionType::ProgramChange => Some(vec![
                    0xC0 + (final_action.channel & 0x0F),
                    final_action.value.unwrap_or(0).clamp(0, 127),
                ]),
            };

            if let Some(msg_bytes) = midi_msg {
                if let Err(e) = handler.send_midi_message(&msg_bytes) {
                    error!("Failed to send merged MIDI message for {}: {:?}", topic, e);
                } else {
                    debug!("Sent merged MIDI message for {}: {:?}", topic, msg_bytes);
                }
            }
        }
    }
}

// Multicast discovery listener
pub async fn run_multicast_discovery_listener(
    main_server_bind_address: String,
) -> Result<(), Box<dyn std::error::Error + Send + Sync + 'static>> {
    info!("Starting multicast discovery listener on {}", MULTICAST_ADDRESS);

    let listen_addr_str = MULTICAST_ADDRESS.split(':').collect::<Vec<&str>>()[1];
    let listen_port: u16 = listen_addr_str.parse()?;
    let listen_ip = "0.0.0.0";

    let socket = UdpSocket::bind(format!("{}:{}", listen_ip, listen_port)).await?;
    
    let multicast_group_addr: Ipv4Addr = MULTICAST_ADDRESS.split(':').collect::<Vec<&str>>()[0].parse()?;
    let interface_to_join_on = Ipv4Addr::new(0,0,0,0);
    socket.join_multicast_v4(multicast_group_addr, interface_to_join_on)?;
    info!("Joined multicast group {} on interface {}", multicast_group_addr, interface_to_join_on);

    let mut buf = [0; 1024];
    loop {
        let (len, src_addr) = socket.recv_from(&mut buf).await?;
        let message = std::str::from_utf8(&buf[..len])?.trim();

        if message == DISCOVERY_MESSAGE {
            info!("Received discovery ping from {}", src_addr);
            let response = format!("{} {}", DISCOVERY_RESPONSE_PREFIX, main_server_bind_address);
            socket.send_to(response.as_bytes(), src_addr).await?;
            info!("Sent discovery response to {}: {}", src_addr, response);
        } else {
            warn!("Received unknown multicast message from {}: {}", src_addr, message);
        }
    }
}

// Main server application logic
pub async fn run_server_application(
    runtime_handle: Handle,
    shutdown_rx: Receiver<()>,
    midi_handler_arc: Arc<Mutex<MidiHandler>>, // Added midi_handler_arc
) -> Result<()> {
    info!("=================================================");
    info!("🚀 Starting SubPub UDP Server v0.1.0");
    info!("=================================================");

    let local_ip = local_ip_address::local_ip().unwrap_or_else(|e| {
        warn!("Could not get local IP address: {}. Defaulting to 127.0.0.1", e);
        IpAddr::V4(Ipv4Addr::new(127, 0, 0, 1))
    });
    
    let port_str = BIND_ADDRESS.split(':').last().unwrap_or("7878");
    let actual_bind_address = format!("{}:{}", local_ip, port_str);

    info!("Attempting to bind main server to: {}", actual_bind_address);

    let socket = Arc::new(UdpSocket::bind(&actual_bind_address).await?);
    let actual_addr = socket.local_addr()?;
    info!("✅ Main server successfully bound and listening on: {}", actual_addr);
    info!("Awaiting incoming UDP messages...");
    info!("-------------------------------------------------");

    let discovery_main_server_addr = actual_addr.to_string();
    runtime_handle.spawn(async move {
        if let Err(e) = run_multicast_discovery_listener(discovery_main_server_addr).await {
            error!("Multicast discovery listener failed: {}", e);
        }
    });

    let subscribers: Subscribers = Arc::new(DashMap::new());

    let server_loop_socket = socket.clone();
    let server_loop_subscribers = subscribers.clone();
    let server_loop_midi_handler = midi_handler_arc.clone(); // Clone for the server loop
    let server_loop_runtime_handle = runtime_handle.clone(); // Clone for the server loop (for NoteOnOff)
    
    let server_task = runtime_handle.spawn(async move {
        if let Err(e) = run_server_processing_loop(
            server_loop_socket, 
            server_loop_subscribers, 
            server_loop_midi_handler,
            server_loop_runtime_handle,
        ).await {
            error!("Server loop exited with error: {}", e);
        }
    });

    shutdown_rx.recv().context("Failed to receive shutdown signal")?;
    info!("Shutdown signal received. Attempting to gracefully shut down server...");
    server_task.abort();
    info!("Server gracefully shut down.");

    Ok(())
}
