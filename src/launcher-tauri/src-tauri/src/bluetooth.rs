//! Bluetooth device pairing, backing src/views/Settings.tsx's Settings >
//! Bluetooth category. Talks to BlueZ over D-Bus via the `bluer` crate --
//! the standard, always-present Bluetooth daemon on any Linux desktop --
//! rather than shelling out to `bluetoothctl`, since pairing and connect
//! state need real async completion signals, not screen-scraped CLI output.
//!
//! Deliberately does NOT register a custom D-Bus pairing agent: BlueZ's
//! default agent (the one GNOME's own Bluetooth settings already registers)
//! handles "Just Works" pairing for the devices this UI targets (headsets,
//! gamepads) with no PIN prompt. Registering a second agent here would
//! fight GNOME's for the DisplayYesNo/RequestPasskey role -- out of scope,
//! and exactly the kind of surprise interaction the owner's Remote Desktop
//! popup objection during this same session says to avoid.

use bluer::{Address, Session};
use futures_util::StreamExt;
use serde::Serialize;
use std::str::FromStr;
use std::time::Duration;

#[derive(Serialize, Clone)]
pub struct BtDevice {
    pub address: String,
    pub name: String,
    pub paired: bool,
    pub connected: bool,
}

async fn default_adapter() -> Result<bluer::Adapter, String> {
    let session = Session::new().await.map_err(|e| e.to_string())?;
    let adapter = session.default_adapter().await.map_err(|e| e.to_string())?;
    adapter.set_powered(true).await.map_err(|e| e.to_string())?;
    Ok(adapter)
}

async fn describe(adapter: &bluer::Adapter, addr: Address) -> Option<BtDevice> {
    let device = adapter.device(addr).ok()?;
    let name = device
        .name()
        .await
        .ok()
        .flatten()
        .unwrap_or_else(|| addr.to_string());
    let paired = device.is_paired().await.unwrap_or(false);
    let connected = device.is_connected().await.unwrap_or(false);
    Some(BtDevice { address: addr.to_string(), name, paired, connected })
}

/// Every device BlueZ currently knows about (paired, or seen in a prior
/// scan) -- the frontend splits this into "paired" / "available" itself by
/// each entry's `paired` flag, mirroring the shape `list_audio_sinks`
/// already establishes for Settings categories.
#[tauri::command]
pub async fn list_bluetooth_devices() -> Result<Vec<BtDevice>, String> {
    let adapter = default_adapter().await?;
    let addrs = adapter.device_addresses().await.map_err(|e| e.to_string())?;
    let mut devices = Vec::new();
    for addr in addrs {
        if let Some(d) = describe(&adapter, addr).await {
            devices.push(d);
        }
    }
    Ok(devices)
}

/// Discovers nearby devices for a fixed window, then returns the same
/// combined list `list_bluetooth_devices` would -- newly-seen unpaired
/// devices included, since starting discovery is what makes BlueZ add
/// them to `device_addresses()` at all.
#[tauri::command]
pub async fn scan_bluetooth_devices() -> Result<Vec<BtDevice>, String> {
    let adapter = default_adapter().await?;
    let mut events = adapter.discover_devices().await.map_err(|e| e.to_string())?;
    let deadline = tokio::time::sleep(Duration::from_secs(8));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            _ = &mut deadline => break,
            ev = events.next() => if ev.is_none() { break },
        }
    }
    list_bluetooth_devices().await
}

#[tauri::command]
pub async fn pair_bluetooth_device(address: String) -> Result<(), String> {
    let addr = Address::from_str(&address).map_err(|e| e.to_string())?;
    let adapter = default_adapter().await?;
    let device = adapter.device(addr).map_err(|e| e.to_string())?;
    if !device.is_paired().await.unwrap_or(false) {
        device.pair().await.map_err(|e| e.to_string())?;
    }
    device.connect().await.map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn connect_bluetooth_device(address: String) -> Result<(), String> {
    let addr = Address::from_str(&address).map_err(|e| e.to_string())?;
    let adapter = default_adapter().await?;
    let device = adapter.device(addr).map_err(|e| e.to_string())?;
    device.connect().await.map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn disconnect_bluetooth_device(address: String) -> Result<(), String> {
    let addr = Address::from_str(&address).map_err(|e| e.to_string())?;
    let adapter = default_adapter().await?;
    let device = adapter.device(addr).map_err(|e| e.to_string())?;
    device.disconnect().await.map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn forget_bluetooth_device(address: String) -> Result<(), String> {
    let addr = Address::from_str(&address).map_err(|e| e.to_string())?;
    let adapter = default_adapter().await?;
    adapter.remove_device(addr).await.map_err(|e| e.to_string())
}
