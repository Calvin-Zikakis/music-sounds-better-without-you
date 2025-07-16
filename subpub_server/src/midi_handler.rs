use anyhow::{Context, Result};
use log::{error, info, warn};
use midir::os::unix::VirtualOutput;
use midir::{MidiOutput, MidiOutputConnection}; // Reverted from wildcard
use serde::{Deserialize, Serialize}; // Added Serialize
use std::collections::HashMap; // Will be useful for quick lookups
use std::fs;
use std::path::Path;
use std::sync::{Arc, Mutex};

const MIDI_CLIENT_NAME: &str = "ZerverClient";
const MAPPING_FILE_PATH: &str = "midi_mapping.toml";

#[derive(Deserialize, Serialize, Debug, Clone)] // Added Serialize
#[serde(rename_all = "snake_case")]
pub enum MidiActionType {
    NoteOn,
    NoteOff,
    NoteOnOff,
    Cc,
    ProgramChange,
}

#[derive(Deserialize, Serialize, Debug, Clone)] // Added Serialize
pub struct MidiAction {
    pub action_type: MidiActionType,
    pub channel: u8, // MIDI channel 0-15 (usually presented as 1-16 to users)
    pub note: Option<u8>,
    pub velocity: Option<u8>,
    #[serde(default)] // If not present, defaults to 0 or a suitable value
    pub duration_ms: Option<u64>,
    pub control_num: Option<u8>,
    pub value: Option<u8>, // Can be direct value or derived from pubsub message
}

#[derive(Deserialize, Serialize, Debug, Clone)] // Added Serialize
pub struct MappingEntry {
    pub sub_topic: String,
    // Optional: further filter by message content (e.g., JSON path, regex)
    // pub message_filter: Option<String>, 
    pub actions: Vec<MidiAction>,
}

#[derive(Deserialize, Serialize, Debug, Clone, Default)] // Added Serialize
pub struct MidiMappingConfig {
    #[serde(default)]
    pub mappings: Vec<MappingEntry>,
}

pub struct MidiHandler {
    conn: Option<MidiOutputConnection>,
    mappings: MidiMappingConfig, // Store loaded mappings
    // For quick lookup of mappings by topic
    topic_to_actions: HashMap<String, Vec<MidiAction>>,
}

impl MidiHandler {
    pub fn new() -> Result<Arc<Mutex<Self>>> {
        let mappings = Self::load_mappings_from_file(Path::new(MAPPING_FILE_PATH))
            .unwrap_or_else(|e| {
                warn!("Failed to load MIDI mappings from '{}': {:?}. Using default empty mappings.", MAPPING_FILE_PATH, e);
                MidiMappingConfig::default()
            });
        
        let topic_to_actions = Self::build_topic_map(&mappings);

        let mut midi_handler = Self { 
            conn: None,
            mappings,
            topic_to_actions,
        };
        match midi_handler.init_midi() {
            Ok(conn) => {
                midi_handler.conn = Some(conn);
                info!("MIDI Handler initialized successfully.");
            }
            Err(e) => {
                error!("Failed to initialize MIDI output: {:?}", e);
                // We can decide if this is a fatal error or if the app can run without MIDI
                // For now, let's let it run but log the error.
            }
        }
        Ok(Arc::new(Mutex::new(midi_handler)))
    }

    fn load_mappings_from_file(path: &Path) -> Result<MidiMappingConfig> {
        if !path.exists() {
            warn!("MIDI mapping file not found at {:?}. Creating a default empty one.", path);
            // Create a default empty TOML file if it doesn't exist
            let default_config = MidiMappingConfig::default();
            let toml_string = toml::to_string_pretty(&default_config)?;
            fs::write(path, toml_string)
                .with_context(|| format!("Failed to write default MIDI mapping file to {:?}", path))?;
            info!("Created default MIDI mapping file at {:?}", path);
            return Ok(default_config);
        }

        let toml_str = fs::read_to_string(path)
            .with_context(|| format!("Failed to read MIDI mapping file from {:?}", path))?;
        let config: MidiMappingConfig = toml::from_str(&toml_str)
            .with_context(|| format!("Failed to parse MIDI mapping TOML from {:?}", path))?;
        info!("Successfully loaded MIDI mappings from {:?}", path);
        Ok(config)
    }
    
    fn build_topic_map(config: &MidiMappingConfig) -> HashMap<String, Vec<MidiAction>> {
        let mut map = HashMap::new();
        for entry in &config.mappings {
            map.insert(entry.sub_topic.clone(), entry.actions.clone());
        }
        map
    }

    pub fn reload_mappings(&mut self) -> Result<()> {
        info!("Attempting to reload MIDI mappings...");
        let new_mappings = Self::load_mappings_from_file(Path::new(MAPPING_FILE_PATH))?;
        self.mappings = new_mappings;
        self.topic_to_actions = Self::build_topic_map(&self.mappings);
        info!("MIDI mappings reloaded successfully.");
        Ok(())
    }

    pub fn get_actions_for_topic(&self, topic: &str) -> Option<Vec<MidiAction>> {
        self.topic_to_actions.get(topic).cloned()
    }

    fn init_midi(&mut self) -> Result<MidiOutputConnection> {
        let midi_out = MidiOutput::new(MIDI_CLIENT_NAME)?;
        
        // For now, let's just create a virtual port.
        // On macOS, this should make it visible to other apps.
        // On Windows, it might require specific drivers or loopMIDI.
        // On Linux, ALSA handles this.
        let port_name = "Zerver";
        let conn = midi_out.create_virtual(port_name).map_err(|e| {
            anyhow::anyhow!("Failed to create virtual MIDI output port with name '{}': {}", port_name, e)
        })?;
        
        info!("Created virtual MIDI output port: {}", port_name);
        Ok(conn)
    }

    pub fn send_midi_message(&mut self, message: &[u8]) -> Result<()> {
        if let Some(conn) = &mut self.conn {
            conn.send(message)
                .with_context(|| "Failed to send MIDI message")?;
            // info!("Sent MIDI: {:?}", message); // Potentially too verbose
        } else {
            // error!("MIDI connection not available. Cannot send message.");
            // This might be too noisy if MIDI failed to init.
            // Consider a state to only log this once or if MIDI was expected.
        }
        Ok(())
    }
}

// Example MIDI messages (Note On, Note Off)
// pub fn note_on(channel: u8, key: u8, velocity: u8) -> [u8; 3] {
//     // Channel is 0-15, so 0x90 + channel
//     [0x90 + (channel & 0x0F), key & 0x7F, velocity & 0x7F]
// }

// pub fn note_off(channel: u8, key: u8, velocity: u8) -> [u8; 3] {
//     // Channel is 0-15, so 0x80 + channel
//     [0x80 + (channel & 0x0F), key & 0x7F, velocity & 0x7F]
// }
