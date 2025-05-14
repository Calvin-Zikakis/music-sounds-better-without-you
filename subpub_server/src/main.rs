use std::collections::HashSet; // HashMap is no longer needed here directly for Subscribers
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::net::UdpSocket;
// Mutex is no longer needed from tokio::sync for Subscribers
use log::{info, warn, error};
use dashmap::DashMap; // Import DashMap

// TODO: Make IP address and port configurable via command-line arguments or a config file.
/// The default IP address and port the UDP server will bind to.
pub const BIND_ADDRESS: &str = "127.0.0.1:7878";

/// A type alias for the shared, thread-safe data structure holding channel subscriptions.
///
/// It uses `DashMap` for concurrent access to channel subscriptions.
/// - The `DashMap` maps channel names (`String`) to a `HashSet` of `SocketAddr`.
/// - The `HashSet` stores the unique addresses of clients subscribed to that channel.
/// `DashMap` provides fine-grained locking, improving performance under contention.
pub type Subscribers = Arc<DashMap<String, HashSet<SocketAddr>>>;

/// Runs the core server logic loop.
///
/// This function listens for incoming UDP datagrams on the provided `socket`.
/// It parses messages according to the pub/sub protocol:
/// - `SUB:channel_name`
/// - `UNSUB:channel_name`
/// - `PUB:channel_name:payload`
///
/// It updates the `subscribers` map based on SUB/UNSUB commands and forwards
/// messages for PUB commands to all subscribed clients on the respective channel.
///
/// # Arguments
/// * `socket`: An `Arc<UdpSocket>` that the server will use to receive and send messages.
/// * `subscribers`: The shared `Subscribers` map to store channel subscriptions.
///
/// # Errors
/// Returns an error if receiving from or sending on the socket fails, or if other
/// critical operational errors occur. The loop itself is designed to run indefinitely
/// unless a critical error forces it to exit.
async fn run_server_processing_loop(
    socket: Arc<UdpSocket>,
    subscribers: Subscribers,
) -> Result<(), Box<dyn std::error::Error + Send + Sync + 'static>> {
    let mut buf = [0; 1024]; // Buffer for incoming data

    loop {
        let (len, addr) = socket.recv_from(&mut buf).await?;
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
            // Optionally send an error back to the client
            // socket.send_to(b"ERROR:InvalidFormat", addr).await?;
            continue;
        }

        let action = parts[0].to_uppercase();
        let channel_name = parts[1].to_string();
        let payload = if parts.len() == 3 { Some(parts[2]) } else { None };

        // No global lock needed for DashMap for individual operations.
        // Operations on DashMap are typically more fine-grained.

        match action.as_str() {
            "SUB" => {
                info!("Client {} subscribed to channel '{}'", addr, channel_name);
                // Get a mutable reference to the HashSet for the channel, creating it if it doesn't exist.
                subscribers.entry(channel_name.clone()).or_default().value_mut().insert(addr);
                // `entry().or_default()` returns a RefMut guard, which is dropped at the end of the statement.
            }
            "UNSUB" => {
                info!("Client {} unsubscribed from channel '{}'", addr, channel_name);
                let mut channel_was_emptied = false;
                if let Some(mut channel_set_ref) = subscribers.get_mut(&channel_name) {
                    // channel_set_ref is a RefMut<String, HashSet<SocketAddr>>
                    let removed = channel_set_ref.value_mut().remove(&addr);
                    if removed && channel_set_ref.value().is_empty() {
                        channel_was_emptied = true;
                    }
                }
                // If the channel became empty after removing the subscriber, remove the channel itself.
                // This is a separate operation to avoid holding a RefMut while trying to remove its key.
                if channel_was_emptied {
                    subscribers.remove(&channel_name);
                    info!("Channel '{}' is now empty and removed.", channel_name);
                }
            }
            "PUB" => {
                if let Some(p) = payload {
                    info!("Client {} published to channel '{}': {}", addr, channel_name, p);
                    let mut subs_to_notify: Vec<SocketAddr> = Vec::new();
                    
                    // Get a read reference to the HashSet for the channel.
                    if let Some(channel_set_ref) = subscribers.get(&channel_name) {
                        // channel_set_ref is a Ref<String, HashSet<SocketAddr>>
                        subs_to_notify = channel_set_ref.value().iter().cloned().collect();
                        // The Ref guard is dropped when channel_set_ref goes out of scope.
                    }

                    if !subs_to_notify.is_empty() {
                        for subscriber_addr in subs_to_notify {
                            if subscriber_addr != addr { // Don't send to self
                                info!("Forwarding message to subscriber {} on channel '{}'", subscriber_addr, channel_name);
                                if let Err(e) = socket.send_to(p.as_bytes(), subscriber_addr).await {
                                    error!("Failed to send message to {}: {}", subscriber_addr, e);
                                    // TODO: Consider removing unresponsive subscribers after several failures
                                }
                            }
                        }
                    } else {
                        info!("No subscribers for channel '{}' or only self. Message not forwarded.", channel_name);
                    }
                } else {
                    warn!("PUB action from {} to channel '{}' without payload.", addr, channel_name);
                }
            }
            // Handle unknown actions
            _ => {
                warn!("Unknown action '{}' from {}: {}", action, addr, message_str);
            }
        }
    }
    // This loop is infinite, so Ok(()) is effectively unreachable.
}

/// The main entry point for the SubPub UDP server application.
///
/// This function performs the following steps:
/// 1. Initializes the logging framework (`env_logger`). It defaults to `info` level
///    for this crate and `warn` for others if `RUST_LOG` is not set.
/// 2. Prints a startup banner and attempts to bind a UDP socket to `BIND_ADDRESS`.
/// 3. If binding is successful, it logs the actual listening address.
/// 4. Initializes the shared `Subscribers` map.
/// 5. Calls `run_server_processing_loop` to start handling client messages.
///
/// # Errors
/// Returns an error if the logger fails to initialize, the socket fails to bind,
/// or if the `run_server_processing_loop` exits with an error.
#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error + Send + Sync + 'static>> {
    // Initialize the logger
    let mut log_builder = env_logger::Builder::from_default_env();
    log_builder.format_timestamp_millis();

    // If RUST_LOG is not set, default to info for this crate and warn for others.
    if std::env::var("RUST_LOG").is_err() {
        log_builder.filter_module(module_path!(), log::LevelFilter::Info)
                   .filter_level(log::LevelFilter::Warn);
    }
    log_builder.init();

    // Startup banner
    info!("=================================================");
    info!("🚀 Starting SubPub UDP Server v0.1.0");
    info!("=================================================");
    info!("Attempting to bind to: {}", BIND_ADDRESS);

    let socket = Arc::new(UdpSocket::bind(BIND_ADDRESS).await?);
    let actual_addr = socket.local_addr()?;
    info!("✅ Server successfully bound and listening on: {}", actual_addr);
    info!("Awaiting incoming UDP messages...");
    info!("-------------------------------------------------");


    let subscribers: Subscribers = Arc::new(DashMap::new()); // Use DashMap

    // Start the main processing loop
    if let Err(e) = run_server_processing_loop(socket, subscribers).await {
        error!("Server loop exited with error: {}", e);
        return Err(e);
    }
    // Normally, run_server_processing_loop runs indefinitely.
    // If it exits, it's likely an unrecoverable error or a planned shutdown (not yet implemented).
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::{IpAddr, Ipv4Addr};
    use std::collections::HashMap; // Keep for process_message_logic test helper
    use env_logger; // For initializing logger in tests if needed, or ensuring it doesn't conflict
    use tokio::time::{sleep, Duration};

    fn create_mock_addr(port: u16) -> SocketAddr {
        SocketAddr::new(IpAddr::V4(Ipv4Addr::new(127, 0, 0, 1)), port)
    }

    // Helper to process a message and update subscribers map for testing core logic
    // This simplifies testing the HashMap manipulation without full async/network setup.
    // This helper remains unchanged as it tests logic against a standard HashMap,
    // which is fine for its specific unit testing purpose.
    fn process_message_logic(
        addr: SocketAddr,
        message_str: &str,
        subs: &mut HashMap<String, HashSet<SocketAddr>>,
        // We'll need a way to capture "sent" messages for PUB tests later,
        // or make this helper focus only on state changes. For now, state.
    ) {
        let parts: Vec<&str> = message_str.splitn(3, ':').collect();
        if parts.len() < 2 {
            return;
        }

        let action = parts[0].to_uppercase();
        let channel_name = parts[1].to_string();
        let _payload = if parts.len() == 3 { Some(parts[2]) } else { None };

        match action.as_str() {
            "SUB" => {
                subs.entry(channel_name.clone()).or_default().insert(addr);
            }
            "UNSUB" => {
                if let Some(channel_subs) = subs.get_mut(&channel_name) {
                    channel_subs.remove(&addr);
                    if channel_subs.is_empty() {
                        subs.remove(&channel_name);
                    }
                }
            }
            "PUB" => { /* No change needed for this helper's scope */ }
            _ => { /* Unknown action */ }
        }
    }

    #[test]
    fn test_subscribe_new_channel() {
        let mut subs = HashMap::new(); // Uses std HashMap for this logic test
        let client_addr = create_mock_addr(10001);
        let channel = "test_channel".to_string();
        process_message_logic(client_addr, &format!("SUB:{}", channel), &mut subs);
        assert!(subs.contains_key(&channel));
        assert_eq!(subs.get(&channel).unwrap().len(), 1);
        assert!(subs.get(&channel).unwrap().contains(&client_addr));
    }

    #[test]
    fn test_subscribe_existing_channel() {
        let mut subs = HashMap::new(); // Uses std HashMap
        let client1_addr = create_mock_addr(10001);
        let client2_addr = create_mock_addr(10002);
        let channel = "test_channel".to_string();
        process_message_logic(client1_addr, &format!("SUB:{}", channel), &mut subs);
        process_message_logic(client2_addr, &format!("SUB:{}", channel), &mut subs);
        assert!(subs.contains_key(&channel));
        assert_eq!(subs.get(&channel).unwrap().len(), 2);
    }

    #[test]
    fn test_subscribe_same_client_twice() {
        let mut subs = HashMap::new(); // Uses std HashMap
        let client_addr = create_mock_addr(10001);
        let channel = "test_channel".to_string();
        process_message_logic(client_addr, &format!("SUB:{}", channel), &mut subs);
        process_message_logic(client_addr, &format!("SUB:{}", channel), &mut subs);
        assert_eq!(subs.get(&channel).unwrap().len(), 1);
    }

    #[test]
    fn test_unsubscribe_from_channel() {
        let mut subs = HashMap::new(); // Uses std HashMap
        let client1_addr = create_mock_addr(10001);
        let client2_addr = create_mock_addr(10002);
        let channel = "test_channel".to_string();
        process_message_logic(client1_addr, &format!("SUB:{}", channel), &mut subs);
        process_message_logic(client2_addr, &format!("SUB:{}", channel), &mut subs);
        process_message_logic(client1_addr, &format!("UNSUB:{}", channel), &mut subs);
        assert_eq!(subs.get(&channel).unwrap().len(), 1);
        assert!(!subs.get(&channel).unwrap().contains(&client1_addr));
    }

    #[test]
    fn test_unsubscribe_last_client_removes_channel() {
        let mut subs = HashMap::new(); // Uses std HashMap
        let client_addr = create_mock_addr(10001);
        let channel = "test_channel".to_string();
        process_message_logic(client_addr, &format!("SUB:{}", channel), &mut subs);
        process_message_logic(client_addr, &format!("UNSUB:{}", channel), &mut subs);
        assert!(!subs.contains_key(&channel));
    }

    #[test]
    fn test_unsubscribe_from_non_existent_channel() {
        let mut subs = HashMap::new(); // Uses std HashMap
        let client_addr = create_mock_addr(10001);
        process_message_logic(client_addr, "UNSUB:test_channel", &mut subs);
        assert!(!subs.contains_key("test_channel"));
    }

    #[test]
    fn test_unsubscribe_non_subscribed_client_from_existing_channel() {
        let mut subs = HashMap::new(); // Uses std HashMap
        let client1_addr = create_mock_addr(10001);
        let client2_addr = create_mock_addr(10002);
        let channel = "test_channel".to_string();
        process_message_logic(client1_addr, &format!("SUB:{}", channel), &mut subs);
        process_message_logic(client2_addr, &format!("UNSUB:{}", channel), &mut subs); // Client 2 was not subscribed
        assert_eq!(subs.get(&channel).unwrap().len(), 1); // Client 1 should still be there
    }
    
    // Helper to get a free port for test server
    async fn bind_to_free_port() -> Result<(Arc<UdpSocket>, SocketAddr), std::io::Error> {
        let socket = UdpSocket::bind("127.0.0.1:0").await?;
        let addr = socket.local_addr()?;
        Ok((Arc::new(socket), addr))
    }

    #[tokio::test]
    async fn test_integration_pub_sub() {
        // Initialize logger for test, if desired (e.g., for debugging test failures)
        // let _ = env_logger::builder().is_test(true).try_init();

        // 1. Start the server logic in a background task on a free port
        let (server_socket, server_addr) = bind_to_free_port().await.expect("Failed to bind server socket");
        // Use DashMap for the integration test's subscriber map
        let server_subscribers: Subscribers = Arc::new(DashMap::new()); 
        
        let server_handle = tokio::spawn(run_server_processing_loop(
            server_socket.clone(), 
            server_subscribers.clone(), // Clone Arc<DashMap>
        ));

        sleep(Duration::from_millis(50)).await; // Allow server to start

        // 2. Create subscriber client
        let subscriber_socket = UdpSocket::bind("127.0.0.1:0").await.expect("Failed to bind subscriber socket");
        let subscriber_addr = subscriber_socket.local_addr().unwrap();
        let channel_name = "news";
        let sub_message = format!("SUB:{}", channel_name);
        subscriber_socket.send_to(sub_message.as_bytes(), server_addr).await.expect("Subscriber failed to send SUB");
        
        sleep(Duration::from_millis(50)).await; // Give time for SUB to be processed

        // Verify subscription on server side
        {
            // Access DashMap directly
            assert!(server_subscribers.get(channel_name).expect("Channel not found after SUB").value().contains(&subscriber_addr));
        }

        // 3. Create publisher client
        let publisher_socket = UdpSocket::bind("127.0.0.1:0").await.expect("Failed to bind publisher socket");
        let pub_payload = "hello world";
        let pub_message = format!("PUB:{}:{}", channel_name, pub_payload);
        publisher_socket.send_to(pub_message.as_bytes(), server_addr).await.expect("Publisher failed to send PUB");

        // 4. Subscriber receives the message
        let mut recv_buf = [0; 1024];
        let timeout_duration = Duration::from_secs(1);

        match tokio::time::timeout(timeout_duration, subscriber_socket.recv_from(&mut recv_buf)).await {
            Ok(Ok((len, _remote_addr))) => {
                let received_payload = std::str::from_utf8(&recv_buf[..len]).unwrap();
                assert_eq!(received_payload, pub_payload);
            }
            Ok(Err(e)) => panic!("Subscriber recv_from failed: {}", e),
            Err(_) => panic!("Subscriber timed out waiting for message"),
        }
        
        // 5. Test UNSUB
        let unsub_message = format!("UNSUB:{}", channel_name);
        subscriber_socket.send_to(unsub_message.as_bytes(), server_addr).await.expect("Subscriber failed to send UNSUB");
        sleep(Duration::from_millis(100)).await; // Give more time for UNSUB and potential remove_if

        // Verify unsubscription
        {
            // Check if channel is gone or subscriber is not in set
            let channel_is_gone_or_client_removed = server_subscribers.get(channel_name)
                .map_or(true, |entry| !entry.value().contains(&subscriber_addr));
            assert!(channel_is_gone_or_client_removed, "Subscriber still present after UNSUB or channel not properly cleaned");
        }

        // Now publish again, subscriber should not receive it
        let pub_message2 = format!("PUB:{}:{}", channel_name, "another message");
        publisher_socket.send_to(pub_message2.as_bytes(), server_addr).await.expect("Publisher failed to send PUB2");
        
        match tokio::time::timeout(Duration::from_millis(200), subscriber_socket.recv_from(&mut recv_buf)).await {
            Ok(Ok((len, _))) => {
                let received_payload = std::str::from_utf8(&recv_buf[..len]).unwrap();
                panic!("Subscriber received message after unsubscribing: {}", received_payload);
            }
            Ok(Err(_e)) => { /* Expected: recv_from error */ },
            Err(_) => { /* Expected: timeout */ info!("Correctly timed out after unsubscribe."); }
        }

        server_handle.abort();
    }
}
