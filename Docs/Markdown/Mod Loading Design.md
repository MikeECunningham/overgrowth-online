---
format: Markdown
...

# Mod Loading Design

*Note that this document is WIP and any thing written here might be changed*

## Rationale

Modding for Overgrowth has become a popular pasttime for many of our fans. This is largely enabled by Angel Script layer giving modders insight and freedom play around.
The current way of modding involved replacing existing content and script-files in the games data folder, usually making mods incompatible and demanding to keep updated as we modify the game and overwrite the modified files. It also creates issues for users who wish to restore their game to the original state. Therefore it's important to simplify modding and make it more modular and easier for the end user.

The mod loading system should fullfill the following features for mods

- Separated to individual folders or packages.
- Have the possibility to co-exist with other mods.
- Remove the necessity to overload or replace existing script files.
- Allow overloading of default files (only one active mod should be able to mask a single file to prevent odd behaviour)
- Enabled to intercommunicate.
- Enabled to have unique names, version numbers.
- Enabled to indicate depencies on other mods.
- Be possible to disable from within the game.
- Automatically detected.
- Be able to specify what version of the game they depend on.

## Overview

## Session mods

Overgrowth Online installs **Difficulty Scaling** and **Friendly Progression** under
`Data/Mods/`. Both are active on first discovery. At the main menu, change either in **Mods**, then start or load a game.
The choice takes effect without restarting the application and survives restarts and updates.
In the lobby, the host still chooses Traditional or Scaling difficulty and whether
to award XP for that mode. Slot ownership, split-screen, host-local save cards
and armor protection work independently of both packages.

The bundled manifests contain numerical tuning under `Rule`, including XP bounties,
shop prices and difficulty gains. Their AngelScript files supply the campaign and
Arena event hooks and campaign shop transitions.

**Difficulty Scaling** contributes the host's difficulty controls, enemy scaling
rules and encounter events. All peers use the host's settings. Saves include
encounter pressure, completion and retry history, and the captured enemy strength.
**Friendly Progression** lets the host choose XP rules for each mode. Kill rewards
start enabled. Damage dealt can also earn XP. Damage taken can cost pooled XP,
and defeat can draw from pooled XP first and committed XP for any shortfall.
Campaign checkpoints and Arena round and match wins have
independent bonuses. An older Arena save that awarded XP for damage keeps its
damage rewards and penalties when loaded. The host can offer a paid XP shop
and free upgrades independently. At each new campaign checkpoint or completed Arena
round the host rolls for each human slot. Luck affects both the chance to receive a
choice and the odds of drawing expensive upgrades. Peers receive the host's rules,
draws, wallets and purchases. Saves keep Luck and pending choices with their unit
slots. Earlier saves start at Luck rank zero and retain any outstanding choices.

### Declare a lobby option

In `mod.xml`, describe the gameplay, network and save effects in the normal
`Description` field. Add a `Session` element for machine-readable compatibility.
This example gives the host a three-choice Arena control in **Mode settings**.

```xml
<SupportsOnline>true</SupportsOnline>
<Session network_version="1" save_version="1">
    <LobbySetting key="session-rules.duration" label="Round length"
                  modes="arena" default="1" description="Choose the length of a round.">
        <Choice>Short</Choice>
        <Choice>Normal</Choice>
        <Choice>Long</Choice>
    </LobbySetting>
</Session>
```

Prefix each setting key with the mod ID and a dot. Values are zero-based indices
into the choices. The host can set preferences per mode, and the engine supplies
the lobby control and preserves the selection across peers, host handoffs and
save/load. A pair of `Off` and `On` choices draws a checkbox. Other choices draw
a dropdown. Omit `modes` for all play modes or comma-separate `campaign`, `arena`
and `sandbox`. To show an option only when another of the mod's settings is
nonzero, use its full key in `depends_on`. Set `live="true"` to allow host scripts
to change the setting during play. The default permits edits in the lobby.

Read the selection with `LobbyGetModeSetting("session-rules.duration")`.
`LobbySetModeSetting(key, value)` checks host authority, mode, timing and range.
With lobby settings, each omitted schema defaults to the mod’s package version.
Bump the network and save schemas when the meaning of an existing index changes.

### Bundle an adapter for an optional mod

A session mod can declare the exact release its compatibility script supports.
For example, Difficulty Scaling uses the following declaration inside its
`Session` element.

```xml
<OnlineAdapter mod="seans-smarter-deaths" version="1.2.5" host_level_hooks="true"/>
```

The player installs and enables the original mod separately. Scripts can check
`ModOnlineAdapterActive(mod_id)` before accessing its settings. When both
packages are active, the lobby accepts the declared release and compares it on
join. The host's save records it as a dependency. Set `host_level_hooks` for
mods whose level hooks change authoritative gameplay state. Their hooks run in
single-player and on the host. A client receives the resulting game state.

To supply a per-object numeric input to a sibling level hook, call
`level.AdaptHookFloatParameter(hook_path, key)` from your hook's `Init`.
Implement `bool AdaptLevelHookFloat(int object_id, const string &in key, float &out value)`.
Return true with the value to expose through that hook's `ScriptParams.HasParam`
and `GetFloat` calls. Return false to use the original parameter. Other hooks
and the editor still read the authored parameters. The registration lasts for
the level and the first adapter to register a given hook and key owns it.
Use `level.GetHookFloat(hook_path, name, value)` to read a live float declared
by that hook. It returns false if the hook or float is unavailable.

### Synchronize and save gameplay data

The host calls `SessionSetModData(mod_id, json_string)` with a JSON object.
It returns whether the write succeeded. Scripts on any peer can read the latest
object using `SessionGetModData(mod_id)`. The empty object is `{}`. Use the stable
lobby slot ID when recording per-player state, so swapping devices or reconnecting
a player preserves ownership. Keep each object's serialized size within 64 KiB
and the total script data within 256 KiB.

- `network_version` includes data in live updates, the initial host snapshot
  and host migration. Joining requires a matching network schema for each active
  session mod. Keep this value when publishing backward-compatible package updates.
- `save_version` includes data in the host's autosave and manual save cards. Load
  checks the installed mod and its save schema before changing the running session.
  A missing or incompatible required mod is identified to the host. Extra active
  mods start with their defaults when a card predates them.

A script mod that only needs stored state can declare just a save schema. A
network-only script mod can declare just a network schema. The usual active-mod
compatibility check still applies. Save-only data stays on the host during play.
A host handoff transfers it to the newly elected host. Use the mod’s own ID for
data access.

## Folder structure

Mods are usually installed in the game Data folder, in the Mods folder, it can look something like this

Overgrowth/Data/Mods/

In the mods folder there is one folder for each mod.

A mod contains at least a mod.xml file. But usually other data as well such as Scripts, Shaders, Models, Textures or Levels, All these folders are contained in the Data folder, resulting in the following structure

~~~
Overgrowth/Data/Mods/
   - my_mod/
       - mod.xml
       - Data/
          - Scripts/
          - Shaders/
          - Models/
          - Textures/
          - Levels/
~~~

## mod.xml - file

The mod.xml file is the primary meta-file for a mod. It's the file that the game searches for and parses to understand how to integrate the mod into the game. Following is a complete example.

    <? xml version="1.0" ?>
    <Mod>
        <!-- The ID is necessary and should never change between versions of the same mod -->
        <Id>unique-name</Id> 
        <!-- The human readable name is presented in UI's -->
        <Name>Human Readable Name</Name> 
        <!-- Version number, can have any formatting you wish. -->
        <Version>1.2.5</Version>

        <Thumbnail>unique_mod.png</Thumbnail>
        
        <!-- Specification for which version of the game that is supported. It's possible to specify more than one and use wildcards -->
        <SupportedVersion>a216</SupportedVersion>
        <SupportedVersion>a215-10*</SupportedVersion>

        <!-- It's possible to specify dependencies of other mods -->
        <ModDependency>
            <Id>other-mod</Id>
            <Version>1.0</Version> <!-- It's possible to specify multiple versions -->
            <Version>1.1</Version>
            <Version>2.*</Version> <!-- And even wildcards -->
        </ModDependency>
        <ModDependency>
            <Id>other-mod2</Id>
            <Version>1.0.0</Version> 
        </ModDependency>

        <!-- Files normally don't replace the standard files of the games, using these it's possible. -->
        <!-- If more than one mod specifies the same file, they are incompatible to run simultaneously -->
        <!-- If a file collides with a system file and isn't listed here it's going to generate an error -->
        <OverloadFile>GLSL/shader.vert</OverloadFile>
        <OverloadFile>Scripts/aschar.as</OverloadFile>

        <!-- Hooks are entrypoints for the engine into the mod -->
        <!-- They are necessary for mods that aren't used through hotspots, levels or similar but instead are always on-->
        <Hook>
            <!-- Available types are ModLoad, LevelLoad, Update, Draw, LevelClose and ModClose, EventReceive-->
            <Type>Update</Type>
            <!-- Path to script file containing the registered function -->
            <File>Scripts/aschar.as</File>
            <!-- Name of the function to be called -->
            <Function>Update</Function>
            <!-- Optional filter for EventReceive calls, these are inclusive, so it's possible to specify multiple. -->
            <Filter>set_player_position</Filter>
            <Filter>change_player_position</Filter>
        </Hook>
        <Hook>
            <Type>ModLoad</Type>
            <File>Scripts/mymodinit.as</File>
            <Function>Init</Function>
        </Hook>
    </Mod>

## Modding related API

### Mod meta data

It's possible to request the state of the loaded mods with the following function.

    array<ModInstance> GetLoadedModsList();

### Message API

One way for mods to globally communicated with eachothers is through the message API. It consists of the following functions.

    ModMessageSend( string message_name, string message );

It's possible to Hook a function to receive messages in a mod. An example is the following

    EventMessageReceive( string mod_name, string message_name, string message );
    
