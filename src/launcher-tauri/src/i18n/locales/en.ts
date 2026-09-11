// The source-of-truth catalog: every other locale file is typed against
// this interface, and any key missing from a translated catalog falls back
// to the English string here (see i18n/index.ts's mergeWithFallback).

/** Lets a translated locale file implement only some keys -- the runtime
 * (i18n/index.ts's mergeWithFallback) already falls back to English for
 * anything missing; locale files should be typed to allow that at compile
 * time too, so a new key can land in English immediately without forcing an
 * edit to all 17 translations in the same change. */
export type DeepPartial<T> = T extends (infer U)[]
  ? DeepPartial<U>[]
  : T extends object
    ? { [K in keyof T]?: DeepPartial<T[K]> }
    : T;

export interface Catalog {
  boot: {
    status: string;
  };
  controlCenter: {
    title: string;
    sound: string;
    profile: string;
    controllers: string;
    settings: string;
    logs: string;
    power: string;
    stop: string;
  };
  power: {
    title: string;
    rest: string;
    quit: string;
    cancel: string;
  };
  exitConfirm: {
    title: string;
    confirm: string;
  };
  restMode: {
    status: string;
  };
  common: {
    cancel: string;
    save: string;
    close: string;
    clear: string;
    back: string;
    defaults: string;
    none: string;
    running: string;
    gamepad: string;
    search: string;
    serialLine: string;
    versionLine: string;
    firmwareLine: string;
  };
  nav: {
    home: string;
    library: string;
    settings: string;
    console: string;
    trophies: string;
    profile: string;
  };
  home: {
    gamesLabel: string;
    play: string;
    stop: string;
    viewDetails: string;
    versionPrefix: string;
    firmwarePrefix: string;
    recentlyPlayed: string;
    hoursPlayed: string;
    statGames: string;
    statPlayed: string;
    statTotalPlaytime: string;
    emptyHint: string;
  };
  library: {
    title: string;
    searchPlaceholder: string;
    searchButton: string;
    inputMapping: string;
    scanning: string;
    all: string;
    showing: string;
    sortByLabel: string;
    sort: {
      title: string;
      nameAsc: string;
      nameDesc: string;
      recent: string;
      played: string;
      addedNew: string;
      addedOld: string;
    };
    filters: {
      button: string;
      title: string;
      status: string;
      played: string;
      playedAll: string;
      playedYes: string;
      playedNo: string;
      customSettings: string;
      reset: string;
      any: string;
    };
    noResults: string;
    emptyHint: string;
    emptyAction: string;
  };
  gameDetail: {
    run: string;
    stop: string;
    compatibilityStatus: string;
    settingsButton: string;
    trophiesButton: string;
    patchesButton: string;
    openFolder: string;
    removeSaveData: string;
    confirmRemoveSaveData: string;
    confirmRemoveSaveDataAction: string;
    removeSaveDataFailed: string;
    saveDataTab: string;
    noSaveData: string;
    status: {
      Unknown: string;
      InGame: string;
      MainMenu: string;
      Logo: string;
      DoesntBoot: string;
    };
  };
  gameSettings: {
    title: string;
    overrideExplanation: string;
    useCustomSettings: string;
    useCustomSettingsHint: string;
    bootExecutable: string;
  };
  settings: {
    categories: { profile: string; defaults: string };
    background: {
      title: string;
      description: string;
      themes: { nebula: string; particles: string; waves: string; aurora: string; deepspace: string; custom: string };
      layout: { title: string; standard: string; library: string };
      displayMode: { title: string; full: string; window: string };
      textSize: { title: string; small: string; standard: string; large: string; extraLarge: string };
    };
    launch: {
      title: string;
      mode: string;
      modeInApp: string;
      modeTerminal: string;
      autoClose: string;
      autoCloseHint: string;
    };
    gamepad: { title: string; description: string; deadzone: string; deadzoneLabel: string; configureButtons: string };
    audio: {
      title: string;
      description: string;
      sfxEnabled: string;
      sfxVolume: string;
      outputDevice: string;
      systemDefault: string;
      noDevicesFound: string;
    };
    bluetooth: {
      title: string;
      description: string;
      scan: string;
      scanning: string;
      noDevices: string;
      connect: string;
      disconnect: string;
      forget: string;
      paired: string;
      available: string;
    };
    folders: { title: string; description: string; none: string; remove: string; add: string };
    defaults: { title: string; save: string; saved: string };
    system: { title: string; description: string; exit: string };
  };
  configForm: {
    screenResolution: string;
    custom: string;
    native: string;
    vblankFrequency: string;
    consoleLanguage: string;
    shaderOptimization: string;
    optionSize: string;
    optionPerformance: string;
    shaderLog: string;
    silent: string;
    file: string;
    shaderLogFolder: string;
    printfOutput: string;
    printfOutputFile: string;
    profiler: string;
    network: string;
    commandBufferDumpFolder: string;
    fullscreen: string;
    vulkanValidation: string;
    shaderValidation: string;
    commandBufferDump: string;
    renderdocCapture: string;
    bvhStub: string;
    /** Index-matched to the PS5 console-language table order (see
     * emulator.rs's --console-language and i18n/languages.ts). */
    consoleLanguages: string[];
  };
  input: {
    title: string;
    inputDevice: string;
    gamepadNotDetectedYet: string;
    keyboardMouse: string;
    helpKeyboard: string;
    helpGamepad: string;
    captureKeyboardPrompt: string;
    captureReservedKey: string;
    captureUnsupportedKey: string;
    captureGamepadButtonPrompt: string;
    captureGamepadAxisPrompt: string;
    noGamepadApi: string;
    label: {
      l2: string; l1: string;
      dpadUp: string; dpadLeft: string; dpadDown: string; dpadRight: string;
      l3: string;
      leftStickUp: string; leftStickDown: string; leftStickLeft: string; leftStickRight: string;
      leftStickX: string; leftStickY: string;
      r2: string; r1: string;
      triangle: string; circle: string; cross: string; square: string;
      options: string; touchPad: string;
      r3: string;
      rightStickUp: string; rightStickDown: string; rightStickLeft: string; rightStickRight: string;
      rightStickX: string; rightStickY: string;
    };
  };
  folderBrowser: {
    title: string;
    selectFolder: string;
    homeButton: string;
    up: string;
    noSubfolders: string;
    gameTag: string;
    imageTitle: string;
    selectImage: string;
    noImages: string;
  };
  patches: {
    title: string;
    applySelection: string;
    selectionSaved: string;
  };
  trophiesView: {
    selectGameHint: string;
    loading: string;
    reward: string;
  };
  trophies: {
    title: string;
    progress: string;
    earned: string;
    platinum: string;
    gold: string;
    silver: string;
    bronze: string;
  };
  profile: {
    description: string;
    language: string;
    timeFormat: string;
    timeFormat24h: string;
    timeFormat12h: string;
    newProfile: string;
    namePlaceholder: string;
    create: string;
    rename: string;
    delete: string;
    active: string;
    cannotDeleteLast: string;
    trophyCount: string;
  };
  profileView: {
    manage: string;
    mostPlayed: string;
    trophiesEarned: string;
    gamesCount: string;
  };
  console: {
    title: string;
    stopButton: string;
    copyLogs: string;
    copiedToClipboard: string;
    lastRunExited: string;
    notRunning: string;
    emptyHint: string;
  };
  topbar: {
    runningTooltip: string;
    aGameIsRunning: string;
    enterFullscreen: string;
    exitFullscreen: string;
    minimize: string;
    restore: string;
    maximize: string;
    exitLauncher: string;
  };
  playtime: {
    lessThanMinute: string;
    minutes: string;
    hours: string;
    hoursMinutes: string;
  };
}

const en: Catalog = {
  boot: {
    status: "Starting Kyty Launcher…",
  },
  controlCenter: {
    title: "Control center",
    sound: "Sound",
    profile: "Profile",
    controllers: "Controllers",
    settings: "Settings",
    logs: "Console",
    power: "Power",
    stop: "Stop",
  },
  power: {
    title: "Power",
    rest: "Enter rest mode",
    quit: "Quit Kyty Launcher",
    cancel: "Cancel",
  },
  exitConfirm: {
    title: "Exit Kyty Launcher?",
    confirm: "Exit launcher",
  },
  restMode: {
    status: "Resting",
  },
  common: {
    cancel: "Cancel",
    save: "Save",
    close: "Close",
    clear: "Clear",
    back: "Back",
    defaults: "Defaults",
    none: "None",
    running: "Running",
    gamepad: "Gamepad",
    search: "Search",
    serialLine: "Serial: {value}",
    versionLine: "Version: {value}",
    firmwareLine: "Firmware: {value}",
  },
  nav: {
    home: "Home",
    library: "Library",
    settings: "Settings",
    console: "Console",
    trophies: "Trophies",
    profile: "Profile",
  },
  home: {
    gamesLabel: "Games",
    play: "Play",
    stop: "Stop",
    viewDetails: "View details",
    versionPrefix: "v{value}",
    firmwarePrefix: "FW {value}",
    recentlyPlayed: "Recently played",
    hoursPlayed: "Hours played",
    statGames: "Games",
    statPlayed: "Played",
    statTotalPlaytime: "Total playtime",
    emptyHint: "No games in your library yet, add a folder from Library to get started.",
  },
  library: {
    title: "Game Library",
    searchPlaceholder: "Search name or serial",
    searchButton: "Search",
    inputMapping: "Input mapping",
    scanning: "Scanning…",
    all: "All: {count}",
    showing: "Showing: {count} of {total}",
    sortByLabel: "Sort by: {value}",
    sort: {
      title: "Sort by",
      nameAsc: "Name (A - Z)",
      nameDesc: "Name (Z - A)",
      recent: "Most recent",
      played: "Most played",
      addedNew: "Date added (New - Old)",
      addedOld: "Date added (Old - New)",
    },
    filters: {
      button: "Sort and filter",
      title: "Filters",
      status: "Status",
      played: "Played",
      playedAll: "All",
      playedYes: "Played",
      playedNo: "Never played",
      customSettings: "Custom settings",
      reset: "Reset filters",
      any: "Any",
    },
    noResults: "No games match your search and filters.",
    emptyHint: "Add at least one game folder to see your library here.",
    emptyAction: "Go to Game folders",
  },
  gameDetail: {
    run: "Run",
    stop: "Stop",
    compatibilityStatus: "Compatibility status",
    settingsButton: "Settings",
    trophiesButton: "Trophies",
    patchesButton: "Patches",
    openFolder: "Open folder",
    removeSaveData: "Remove saved data",
    confirmRemoveSaveData: 'Remove saved data for "{name}"? This cannot be undone.',
    confirmRemoveSaveDataAction: "Remove",
    removeSaveDataFailed: "Could not remove:\n{list}",
    saveDataTab: "Saved data",
    noSaveData: "No saved data found for this game.",
    status: {
      Unknown: "Unknown",
      InGame: "In game",
      MainMenu: "Main menu",
      Logo: "Logo only",
      DoesntBoot: "Doesn't boot",
    },
  },
  gameSettings: {
    title: "Game settings, {name}",
    overrideExplanation: "These settings override the global Emulator settings for this game only.",
    useCustomSettings: "Use custom settings for this game",
    useCustomSettingsHint: "When off, this game uses the global Emulator settings instead.",
    bootExecutable: "Boot executable (relative to game folder)",
  },
  settings: {
    categories: {
      profile: "Profile",
      defaults: "Emulator settings",
    },
    background: {
      title: "Appearance",
      description: "Choose the background shown when no game is selected, and behind Settings, Library and Profile",
      themes: {
        nebula: "Nebula Drift",
        particles: "Particle Field",
        waves: "Geometric Waves",
        aurora: "Aurora Glow",
        deepspace: "Deep Space",
        custom: "Custom",
      },
      layout: {
        title: "Choose home layout",
        standard: "Minimal layout",
        library: "Gaming library layout",
      },
      displayMode: {
        title: "Display mode",
        full: "Full Mode",
        window: "Window Mode",
      },
      textSize: {
        title: "Text size",
        small: "Small",
        standard: "Standard",
        large: "Large",
        extraLarge: "Extra large",
      },
    },
    launch: {
      title: "Launch",
      mode: "Launch mode",
      modeInApp: "In-app console (recommended)",
      modeTerminal: "External terminal",
      autoClose: "Close launcher when a game starts",
      autoCloseHint:
        "Frees the launcher's own memory and CPU while you play, then reopens it automatically once the game closes. In-app launch mode only.",
    },
    gamepad: {
      title: "Gamepad",
      description:
        "Any connected controller already works out of the box via SDL2, no setup needed. Remap buttons or tune the deadzone only if you want something different from the default gamepad layout.",
      deadzone: "Stick deadzone ({percent}%)",
      deadzoneLabel: "Stick deadzone",
      configureButtons: "Configure gamepad buttons…",
    },
    audio: {
      title: "Audio",
      description: "UI sound effects and where this app's audio, and the emulator's, plays out of.",
      sfxEnabled: "UI sound effects",
      sfxVolume: "Effects volume ({percent}%)",
      outputDevice: "Output device",
      systemDefault: "System default",
      noDevicesFound: "No audio devices found.",
    },
    bluetooth: {
      title: "Bluetooth",
      description: "Pair audio devices and gamepads. Once paired, a controller works from this app's own gamepad navigation without touching a mouse.",
      scan: "Scan for devices",
      scanning: "Scanning…",
      noDevices: "No devices found yet.",
      connect: "Connect",
      disconnect: "Disconnect",
      forget: "Forget",
      paired: "Paired devices",
      available: "Available devices",
    },
    folders: {
      title: "Game folders",
      description: "Folders the library scans for games",
      none: "No folders configured.",
      remove: "Remove",
      add: "+ Add folder",
    },
    defaults: {
      title: "Default emulator settings for newly scanned games",
      save: "Save",
      saved: "Saved.",
    },
    system: {
      title: "System",
      description: "Launcher-level actions",
      exit: "Exit Kyty Launcher",
    },
  },
  configForm: {
    screenResolution: "Screen resolution",
    custom: "Custom…",
    native: "native",
    vblankFrequency: "Vblank frequency (Hz)",
    consoleLanguage: "Console language",
    shaderOptimization: "Shader optimization",
    optionSize: "Size",
    optionPerformance: "Performance",
    shaderLog: "Shader log",
    silent: "Silent",
    file: "File",
    shaderLogFolder: "Shader log folder",
    printfOutput: "Printf output",
    printfOutputFile: "Printf output file",
    profiler: "Profiler",
    network: "Network",
    commandBufferDumpFolder: "Command buffer dump folder",
    fullscreen: "Fullscreen",
    vulkanValidation: "Vulkan validation",
    shaderValidation: "Shader validation",
    commandBufferDump: "Command buffer dump",
    renderdocCapture: "RenderDoc capture",
    bvhStub: "Ray-tracing stub (experimental, inaccurate)",
    consoleLanguages: [
      "Japanese", "English (United States)", "French (France)", "Spanish (Spain)", "German",
      "Italian", "Dutch", "Portuguese (Portugal)", "Russian", "Korean", "Chinese (Traditional)",
      "Chinese (Simplified)", "Finnish", "Swedish", "Danish", "Norwegian", "Polish",
      "Portuguese (Brazil)", "English (United Kingdom)", "Turkish", "Spanish (Latin America)",
      "Arabic", "French (Canada)", "Czech", "Hungarian", "Greek", "Romanian", "Thai",
      "Vietnamese", "Indonesian",
    ],
  },
  input: {
    title: "Controller mapping",
    inputDevice: "Input Device",
    gamepadNotDetectedYet: "Gamepad (press any button to detect)",
    keyboardMouse: "Keyboard & mouse",
    helpKeyboard: "Select a label to capture a new key or mouse binding for it.",
    helpGamepad:
      "Select a label, then press the button or move the stick on your controller. Leave everything unset to use the built-in default gamepad layout.",
    captureKeyboardPrompt: "Press a key or mouse button. Space and F1 are reserved; Esc cancels.",
    captureReservedKey: "That key is reserved by the emulator.",
    captureUnsupportedKey: "That key is not supported.",
    captureGamepadButtonPrompt: "Press a button on your gamepad. Esc cancels.",
    captureGamepadAxisPrompt: "Move the stick fully, or pull the trigger. Esc cancels.",
    noGamepadApi: "No gamepad input is available on this system.",
    label: {
      l2: "L2 (trigger)", l1: "L1 (bumper)",
      dpadUp: "D-pad Up", dpadLeft: "D-pad Left", dpadDown: "D-pad Down", dpadRight: "D-pad Right",
      l3: "L3 (stick click)",
      leftStickUp: "Left stick ↑", leftStickDown: "Left stick ↓", leftStickLeft: "Left stick ←", leftStickRight: "Left stick →",
      leftStickX: "Left stick ↔", leftStickY: "Left stick ↕",
      r2: "R2 (trigger)", r1: "R1 (bumper)",
      triangle: "Triangle", circle: "Circle", cross: "Cross", square: "Square",
      options: "Options", touchPad: "Touch pad",
      r3: "R3 (stick click)",
      rightStickUp: "Right stick ↑", rightStickDown: "Right stick ↓", rightStickLeft: "Right stick ←", rightStickRight: "Right stick →",
      rightStickX: "Right stick ↔", rightStickY: "Right stick ↕",
    },
  },
  folderBrowser: {
    title: "Choose a folder",
    selectFolder: "Select this folder",
    homeButton: "Home",
    up: "Up",
    noSubfolders: "No subfolders here.",
    gameTag: "GAME",
    imageTitle: "Choose a background image",
    selectImage: "Select image",
    noImages: "No folders or images here.",
  },
  patches: {
    title: "Patches (experimental), {name}",
    applySelection: "Apply selection",
    selectionSaved: "Patch selection saved.",
  },
  trophiesView: {
    selectGameHint: "Select a game in the Library, then open its trophies from the detail pane.",
    loading: "Loading trophies…",
    reward: "Reward: {value}",
  },
  trophies: {
    title: "Trophies",
    progress: "Progress",
    earned: "Earned",
    platinum: "Platinum",
    gold: "Gold",
    silver: "Silver",
    bronze: "Bronze",
  },
  profile: {
    description: "Local profiles, language and gamepad input",
    language: "Language",
    timeFormat: "Time format",
    timeFormat24h: "24-hour",
    timeFormat12h: "12-hour",
    newProfile: "+ New profile",
    namePlaceholder: "Profile name",
    create: "Create",
    rename: "Rename",
    delete: "Delete",
    active: "Active",
    cannotDeleteLast: "At least one profile must remain.",
    trophyCount: "{count} trophies",
  },
  profileView: {
    manage: "Manage profiles",
    mostPlayed: "Most played",
    trophiesEarned: "Trophies earned: {value}",
    gamesCount: "Games: {value}",
  },
  console: {
    title: "Emulator console",
    stopButton: "Stop",
    copyLogs: "Copy logs",
    copiedToClipboard: "Logs copied to clipboard",
    lastRunExited: "Last run exited with code {code}",
    notRunning: "Not running",
    emptyHint: "No output yet, run a game to see its console here.",
  },
  topbar: {
    runningTooltip: "Running: {name}",
    aGameIsRunning: "A game is running",
    enterFullscreen: "Enter full screen",
    exitFullscreen: "Exit full screen",
    minimize: "Minimize",
    restore: "Restore",
    maximize: "Maximize",
    exitLauncher: "Exit launcher",
  },
  playtime: {
    lessThanMinute: "< 1 min",
    minutes: "{count} min",
    hours: "{count} h",
    hoursMinutes: "{hours} h {minutes} min",
  },
};

export default en;
