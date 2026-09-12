//! Bluetooth device pairing, backing src/views/Settings.tsx's Settings >
//! Bluetooth category.
//!
//! There is no portable backend to reach for. Every cross-platform Rust
//! Bluetooth crate is BLE-only, while the devices this panel exists for --
//! DualSense pads, headsets -- pair over Bluetooth Classic (BR/EDR). So
//! each OS gets its own `platform` module below, talking to that system's
//! native stack:
//!
//! - Linux: BlueZ over D-Bus through `bluer`, rather than screen-scraping
//!   `bluetoothctl`, since pairing and connect state need real async
//!   completion signals. Deliberately does NOT register a custom D-Bus
//!   pairing agent: BlueZ's default agent (the one GNOME's own Bluetooth
//!   settings already registers) handles "Just Works" pairing for these
//!   devices with no PIN prompt, and a second agent would fight GNOME's for
//!   the DisplayYesNo/RequestPasskey role.
//! - Windows: WinRT device enumeration (`Windows.Devices.Enumeration`),
//!   filtered to the Bluetooth association endpoints. Pairing goes through
//!   `DeviceInformationPairing`, which raises the system's own consent UI
//!   for devices that need one.
//! - macOS: IOBluetooth. `IOBluetoothDevice::pairedDevices` for the list,
//!   `IOBluetoothDevicePair` to pair.
//!
//! All four modules expose the same six functions, so the commands at the
//! bottom of this file are platform-independent and stay registered on
//! every target.

use serde::Serialize;

#[derive(Serialize, Clone)]
pub struct BtDevice {
    pub address: String,
    pub name: String,
    pub paired: bool,
    pub connected: bool,
}

/// Whether this machine can do Bluetooth at all, and whether the radio is
/// currently switched on. The two are separate answers on purpose: a
/// machine with no adapter and a machine whose adapter is turned off both
/// produce an empty device list, but only one of them is worth telling the
/// user to go and fix.
#[derive(Serialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct BtAdapter {
    /// An adapter exists on this machine.
    pub present: bool,
    /// The adapter exists and its radio is on.
    pub powered: bool,
}

impl BtAdapter {
    fn missing() -> Self {
        BtAdapter { present: false, powered: false }
    }
}

#[cfg(target_os = "linux")]
mod platform {
    use super::{BtAdapter, BtDevice};
    use bluer::{Address, Session};
    use futures_util::StreamExt;
    use std::str::FromStr;
    use std::time::Duration;

    /// Deliberately does not go through `default_adapter`, which powers the
    /// adapter on: asking whether the radio is on must not be what turns it
    /// on.
    pub async fn adapter_state() -> BtAdapter {
        let Ok(adapter) = read_only_adapter().await else { return BtAdapter::missing() };
        BtAdapter { present: true, powered: adapter.is_powered().await.unwrap_or(false) }
    }

    /// For actions the user explicitly asked for (scanning, pairing,
    /// connecting), which cannot work with the radio off -- so this powers
    /// it on. Merely *listing* devices must not, or opening the Bluetooth
    /// settings page would silently switch the user's radio on behind them;
    /// `read_only_adapter` is for that.
    pub async fn default_adapter() -> Result<bluer::Adapter, String> {
        let adapter = read_only_adapter().await?;
        adapter.set_powered(true).await.map_err(|e| e.to_string())?;
        Ok(adapter)
    }

    async fn read_only_adapter() -> Result<bluer::Adapter, String> {
        let session = Session::new().await.map_err(|e| e.to_string())?;
        session.default_adapter().await.map_err(|e| e.to_string())
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

    pub async fn list() -> Result<Vec<BtDevice>, String> {
        let adapter = read_only_adapter().await?;
        let addrs = adapter.device_addresses().await.map_err(|e| e.to_string())?;
        let mut devices = Vec::new();
        for addr in addrs {
            if let Some(d) = describe(&adapter, addr).await {
                devices.push(d);
            }
        }
        Ok(devices)
    }

    pub async fn scan() -> Result<Vec<BtDevice>, String> {
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
        list().await
    }

    async fn device(address: &str) -> Result<bluer::Device, String> {
        let addr = Address::from_str(address).map_err(|e| e.to_string())?;
        let adapter = default_adapter().await?;
        adapter.device(addr).map_err(|e| e.to_string())
    }

    pub async fn pair(address: String) -> Result<(), String> {
        let device = device(&address).await?;
        if !device.is_paired().await.unwrap_or(false) {
            device.pair().await.map_err(|e| e.to_string())?;
        }
        device.connect().await.map_err(|e| e.to_string())
    }

    pub async fn connect(address: String) -> Result<(), String> {
        device(&address).await?.connect().await.map_err(|e| e.to_string())
    }

    pub async fn disconnect(address: String) -> Result<(), String> {
        device(&address).await?.disconnect().await.map_err(|e| e.to_string())
    }

    pub async fn forget(address: String) -> Result<(), String> {
        let addr = Address::from_str(&address).map_err(|e| e.to_string())?;
        let adapter = default_adapter().await?;
        adapter.remove_device(addr).await.map_err(|e| e.to_string())
    }
}

#[cfg(target_os = "windows")]
mod platform {
    use super::{BtAdapter, BtDevice};
    use windows::Devices::Bluetooth::{BluetoothAdapter, BluetoothConnectionStatus, BluetoothDevice};
    use windows::Devices::Radios::RadioState;
    use windows::Devices::Enumeration::{
        DeviceInformation, DevicePairingResultStatus, DeviceUnpairingResultStatus,
    };

    /// WinRT objects are apartment-bound and not `Send`, and every call here
    /// has to hold one across an `.await` on the matching `IAsyncOperation`.
    /// Tauri needs the command futures to be `Send`, so the WinRT half runs
    /// on a blocking thread with its own single-threaded runtime, and only
    /// the plain `BtDevice` data crosses back. These are user-initiated and
    /// infrequent, so standing a runtime up per call is not worth avoiding.
    async fn off_thread<T, F, Fut>(work: F) -> Result<T, String>
    where
        T: Send + 'static,
        F: FnOnce() -> Fut + Send + 'static,
        Fut: std::future::Future<Output = Result<T, String>>,
    {
        tauri::async_runtime::spawn_blocking(move || {
            tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .map_err(|e| e.to_string())?
                .block_on(work())
        })
        .await
        .map_err(|e| e.to_string())?
    }

    /// WinRT hands back a Bluetooth address as a plain 48-bit integer, which
    /// is not what anyone recognizes as a device address, and not what the
    /// other commands here are given to look one up by.
    fn format_address(address: u64) -> String {
        let b = address.to_be_bytes();
        format!(
            "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
            b[2], b[3], b[4], b[5], b[6], b[7]
        )
    }

    /// Device ids look like `Bluetooth#Bluetooth<host>-<remote>`, so the
    /// address is the tail. Only used when opening the `BluetoothDevice`
    /// fails, which happens for devices seen in a scan but not yet paired:
    /// listing one without an address would give the user a row they cannot
    /// then act on.
    fn address_from_id(id: &str) -> Option<String> {
        let tail = id.rsplit('-').next()?;
        let cleaned: String = tail.chars().filter(|c| c.is_ascii_hexdigit()).collect();
        (cleaned.len() == 12).then(|| {
            cleaned
                .as_bytes()
                .chunks(2)
                .map(|pair| String::from_utf8_lossy(pair).to_uppercase())
                .collect::<Vec<_>>()
                .join(":")
        })
    }

    /// `paired` comes from the selector the device was found through, not
    /// from `info.Pairing().IsPaired()`. On the `DeviceInformation` behind a
    /// `BluetoothDevice` that property reports false even for devices
    /// Windows itself lists as paired (it tracks the association endpoint,
    /// which is a different object), so trusting it put every paired device
    /// in the "available" half of the panel, offering Connect on something
    /// already paired.
    async fn describe(info: &DeviceInformation, paired: bool) -> Option<BtDevice> {
        let id = info.Id().ok()?;
        let name = info
            .Name()
            .map(|n| n.to_string_lossy())
            .ok()
            .filter(|n| !n.is_empty());

        // The full device object carries the address and the live connection
        // state, but it can only be opened for devices Windows has a real
        // radio association with, so fall back to the id for the rest.
        match BluetoothDevice::FromIdAsync(&id).ok()?.await {
            Ok(device) => {
                let address = format_address(device.BluetoothAddress().ok()?);
                Some(BtDevice {
                    name: name
                        .or_else(|| device.Name().map(|n| n.to_string_lossy()).ok())
                        .unwrap_or_else(|| address.clone()),
                    connected: device
                        .ConnectionStatus()
                        .map(|s| s == BluetoothConnectionStatus::Connected)
                        .unwrap_or(false),
                    paired,
                    address,
                })
            }
            Err(_) => {
                let address = address_from_id(&id.to_string_lossy())?;
                Some(BtDevice {
                    name: name.unwrap_or_else(|| address.clone()),
                    address,
                    paired,
                    connected: false,
                })
            }
        }
    }

    /// One enumeration pass over devices in the given pairing state. This is
    /// a snapshot rather than a live `DeviceWatcher`: a watcher would suit a
    /// panel that fills in as devices answer, but these commands are
    /// request/response over IPC with nowhere to push later updates, so the
    /// frontend re-invokes instead.
    async fn enumerate(paired: bool) -> Result<Vec<BtDevice>, String> {
        let selector = BluetoothDevice::GetDeviceSelectorFromPairingState(paired).map_err(|e| e.message())?;
        let collection = DeviceInformation::FindAllAsyncAqsFilter(&selector)
            .map_err(|e| e.message())?
            .await
            .map_err(|e| e.message())?;

        let mut devices = Vec::new();
        for info in collection {
            if let Some(device) = describe(&info, paired).await {
                devices.push(device);
            }
        }
        Ok(devices)
    }

    async fn find_by_address(address: &str) -> Result<DeviceInformation, String> {
        let wanted = address.to_uppercase();
        for paired in [true, false] {
            let selector =
                BluetoothDevice::GetDeviceSelectorFromPairingState(paired).map_err(|e| e.message())?;
            let collection = DeviceInformation::FindAllAsyncAqsFilter(&selector)
                .map_err(|e| e.message())?
                .await
                .map_err(|e| e.message())?;
            for info in collection {
                if describe(&info, paired).await.map(|d| d.address) == Some(wanted.clone()) {
                    return Ok(info);
                }
            }
        }
        Err(format!("No Bluetooth device with address {address} was found."))
    }

    /// `GetDefaultAsync` resolves to nothing when the machine has no radio;
    /// the radio's own on/off switch is a separate object, reached through
    /// the adapter, so a present-but-disabled adapter still answers here.
    pub async fn adapter_state() -> BtAdapter {
        off_thread(|| async {
            let Ok(operation) = BluetoothAdapter::GetDefaultAsync() else {
                return Ok(BtAdapter::missing());
            };
            let Ok(adapter) = operation.await else {
                return Ok(BtAdapter::missing());
            };
            let powered = match adapter.GetRadioAsync() {
                Ok(radio) => radio
                    .await
                    .ok()
                    .and_then(|r| r.State().ok())
                    .map(|state| state == RadioState::On)
                    .unwrap_or(false),
                Err(_) => false,
            };
            Ok(BtAdapter { present: true, powered })
        })
        .await
        .unwrap_or_else(|_| BtAdapter::missing())
    }

    pub async fn list() -> Result<Vec<BtDevice>, String> {
        off_thread(|| enumerate(true)).await
    }

    /// Paired devices plus whatever is in range but unpaired, which is the
    /// half a scan exists to surface.
    pub async fn scan() -> Result<Vec<BtDevice>, String> {
        off_thread(|| async {
            let mut devices = enumerate(true).await?;
            let known: Vec<String> = devices.iter().map(|d| d.address.clone()).collect();
            for device in enumerate(false).await? {
                if !known.contains(&device.address) {
                    devices.push(device);
                }
            }
            Ok(devices)
        })
        .await
    }

    pub async fn pair(address: String) -> Result<(), String> {
        off_thread(move || async move {
            let pairing = find_by_address(&address).await?.Pairing().map_err(|e| e.message())?;
            if pairing.IsPaired().unwrap_or(false) {
                return Ok(());
            }
            let result = pairing
                .PairAsync()
                .map_err(|e| e.message())?
                .await
                .map_err(|e| e.message())?;
            match result.Status().map_err(|e| e.message())? {
                DevicePairingResultStatus::Paired | DevicePairingResultStatus::AlreadyPaired => Ok(()),
                other => Err(format!("Pairing failed ({other:?}).")),
            }
        })
        .await
    }

    /// Windows exposes no app-facing "connect this paired device" call for
    /// Bluetooth Classic -- the profile drivers bring the link up themselves
    /// once the device is paired and in range. Saying so beats a button that
    /// silently does nothing.
    pub async fn connect(_address: String) -> Result<(), String> {
        Err("Windows reconnects paired Bluetooth devices by itself; switch the device on and it will connect."
            .to_string())
    }

    pub async fn disconnect(_address: String) -> Result<(), String> {
        Err("Windows does not let an app disconnect a paired Bluetooth device; use Remove instead.".to_string())
    }

    pub async fn forget(address: String) -> Result<(), String> {
        off_thread(move || async move {
            let pairing = find_by_address(&address).await?.Pairing().map_err(|e| e.message())?;
            let result = pairing
                .UnpairAsync()
                .map_err(|e| e.message())?
                .await
                .map_err(|e| e.message())?;
            match result.Status().map_err(|e| e.message())? {
                DeviceUnpairingResultStatus::Unpaired | DeviceUnpairingResultStatus::AlreadyUnpaired => Ok(()),
                other => Err(format!("Removing the pairing failed ({other:?}).")),
            }
        })
        .await
    }
}

#[cfg(target_os = "macos")]
mod platform {
    use super::{BtAdapter, BtDevice};
    use objc2_foundation::{NSArray, NSString};
    use objc2_io_bluetooth::{BluetoothHCIPowerState, IOBluetoothDevice, IOBluetoothDevicePair, IOBluetoothHostController};

    /// `IOReturn` is a plain status code; every call here treats zero
    /// (`kIOReturnSuccess`) as the only success.
    const IO_RETURN_SUCCESS: i32 = 0;

    fn check(status: i32, what: &str) -> Result<(), String> {
        if status == IO_RETURN_SUCCESS {
            Ok(())
        } else {
            Err(format!("{what} failed (IOReturn {status})."))
        }
    }

    /// Everything Objective-C stays inside these sync helpers. The async
    /// wrappers below hold no `Retained` handle across an await, which keeps
    /// the futures `Send` for the command layer.
    fn describe(device: &IOBluetoothDevice) -> Option<BtDevice> {
        // Without an address there is nothing the other commands could look
        // the device up by later, so the row would be dead weight.
        let address = unsafe { device.addressString() }?.to_string();
        let name = unsafe { device.nameOrAddress() }
            .map(|n| n.to_string())
            .unwrap_or_else(|| address.clone());
        Some(BtDevice {
            address,
            name,
            paired: unsafe { device.isPaired() },
            connected: unsafe { device.isConnected() },
        })
    }

    fn collect(array: Option<objc2::rc::Retained<NSArray>>) -> Vec<BtDevice> {
        let Some(array) = array else { return Vec::new() };
        let mut devices = Vec::new();
        for index in 0..array.count() {
            let object = array.objectAtIndex(index);
            if let Ok(device) = object.downcast::<IOBluetoothDevice>() {
                if let Some(described) = describe(&device) {
                    devices.push(described);
                }
            }
        }
        devices
    }

    fn find(address: &str) -> Result<objc2::rc::Retained<IOBluetoothDevice>, String> {
        let key = NSString::from_str(address);
        unsafe { IOBluetoothDevice::deviceWithAddressString(Some(&key)) }
            .ok_or_else(|| format!("No Bluetooth device with address {address} was found."))
    }

    fn list_sync() -> Vec<BtDevice> {
        collect(unsafe { IOBluetoothDevice::pairedDevices() })
    }

    /// macOS has no scan that fits here. A live inquiry means
    /// `IOBluetoothDeviceInquiry`, which reports results to an Objective-C
    /// delegate and needs a CFRunLoop turning on the calling thread -- these
    /// commands run on a worker thread with no run loop, so an inquiry would
    /// simply never report anything. `recentDevices` is the honest
    /// substitute: devices the system has seen before, paired or not.
    fn scan_sync() -> Vec<BtDevice> {
        let mut devices = list_sync();
        let known: Vec<String> = devices.iter().map(|d| d.address.clone()).collect();
        for device in collect(unsafe { IOBluetoothDevice::recentDevices(0) }) {
            if !known.contains(&device.address) {
                devices.push(device);
            }
        }
        devices
    }

    fn pair_sync(address: &str) -> Result<(), String> {
        let device = find(address)?;
        if unsafe { device.isPaired() } {
            return Ok(());
        }
        let pair = unsafe { IOBluetoothDevicePair::pairWithDevice(Some(&device)) }
            .ok_or_else(|| "Could not start pairing with this device.".to_string())?;
        // No delegate is set, so this covers "Just Works" devices (pads,
        // headsets) only. Anything demanding a PIN or numeric confirmation
        // needs a delegate to answer the challenge, which would mean an
        // Objective-C class and a run loop to deliver the callbacks on.
        check(unsafe { pair.start() }, "Pairing")
    }

    fn connect_sync(address: &str) -> Result<(), String> {
        check(unsafe { find(address)?.openConnection() }, "Connecting")
    }

    fn disconnect_sync(address: &str) -> Result<(), String> {
        check(unsafe { find(address)?.closeConnection() }, "Disconnecting")
    }

    /// `defaultController` returns nothing when the Mac has no Bluetooth
    /// hardware at all; a controller that exists but is switched off still
    /// answers, with a power state of OFF.
    pub async fn adapter_state() -> BtAdapter {
        let Some(controller) = (unsafe { IOBluetoothHostController::defaultController() }) else {
            return BtAdapter::missing();
        };
        BtAdapter {
            present: true,
            powered: unsafe { controller.powerState() } == BluetoothHCIPowerState::ON,
        }
    }

    pub async fn list() -> Result<Vec<BtDevice>, String> {
        Ok(list_sync())
    }

    pub async fn scan() -> Result<Vec<BtDevice>, String> {
        Ok(scan_sync())
    }

    pub async fn pair(address: String) -> Result<(), String> {
        pair_sync(&address)
    }

    pub async fn connect(address: String) -> Result<(), String> {
        connect_sync(&address)
    }

    pub async fn disconnect(address: String) -> Result<(), String> {
        disconnect_sync(&address)
    }

    /// IOBluetooth exposes no supported way to remove a pairing: the one
    /// call that did (`IOBluetoothDevice::remove`) has never been public
    /// API. Saying so beats a button that reports success and changes
    /// nothing.
    pub async fn forget(_address: String) -> Result<(), String> {
        Err("macOS does not let an app remove a Bluetooth pairing; use System Settings > Bluetooth.".to_string())
    }
}

#[cfg(not(any(target_os = "linux", target_os = "windows", target_os = "macos")))]
mod platform {
    use super::{BtAdapter, BtDevice};

    pub async fn adapter_state() -> BtAdapter {
        BtAdapter::missing()
    }

    /// Platforms with no backend here. An explicit error beats an empty
    /// list, which in the UI is indistinguishable from an adapter that is
    /// simply switched off.
    fn unsupported<T>() -> Result<T, String> {
        Err("Bluetooth is not supported on this platform in this launcher.".to_string())
    }

    pub async fn list() -> Result<Vec<BtDevice>, String> {
        unsupported()
    }

    pub async fn scan() -> Result<Vec<BtDevice>, String> {
        unsupported()
    }

    pub async fn pair(_address: String) -> Result<(), String> {
        unsupported()
    }

    pub async fn connect(_address: String) -> Result<(), String> {
        unsupported()
    }

    pub async fn disconnect(_address: String) -> Result<(), String> {
        unsupported()
    }

    pub async fn forget(_address: String) -> Result<(), String> {
        unsupported()
    }
}

/// Whether this machine has a Bluetooth adapter and whether it is switched
/// on. The panel asks first, so it can say "turn Bluetooth on" or "no
/// adapter" instead of showing an empty device list that looks the same in
/// both cases. Never fails: a machine with no Bluetooth is a normal answer,
/// not an error.
#[tauri::command]
pub async fn bluetooth_adapter_state() -> BtAdapter {
    platform::adapter_state().await
}

/// Every device BlueZ currently knows about (paired, or seen in a prior
/// scan) -- the frontend splits this into "paired" / "available" itself by
/// each entry's `paired` flag, mirroring the shape `list_audio_sinks`
/// already establishes for Settings categories.
#[tauri::command]
pub async fn list_bluetooth_devices() -> Result<Vec<BtDevice>, String> {
    platform::list().await
}

/// Discovers nearby devices for a fixed window, then returns the same
/// combined list `list_bluetooth_devices` would -- newly-seen unpaired
/// devices included, since starting discovery is what makes BlueZ add
/// them to `device_addresses()` at all.
#[tauri::command]
pub async fn scan_bluetooth_devices() -> Result<Vec<BtDevice>, String> {
    platform::scan().await
}

#[tauri::command]
pub async fn pair_bluetooth_device(address: String) -> Result<(), String> {
    platform::pair(address).await
}

#[tauri::command]
pub async fn connect_bluetooth_device(address: String) -> Result<(), String> {
    platform::connect(address).await
}

#[tauri::command]
pub async fn disconnect_bluetooth_device(address: String) -> Result<(), String> {
    platform::disconnect(address).await
}

#[tauri::command]
pub async fn forget_bluetooth_device(address: String) -> Result<(), String> {
    platform::forget(address).await
}
