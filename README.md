# Stack MagicQ Plugin

This is a plugin for [Stack](https://github.com/claytonpeters/stack) that
provides a cue type for interacting with Chamsys MagicQ software. The plugin
relies on OSC being available from MagicQwhich is only available for use in the
full version of MagicQ (i.e. not when in "Demo Mode" as is the normal for MagicQ
PC with a MagicDMX dongle attached). Each cue can perform one or more actions as
allowed by the protocol (via the ChamSys Remote Ethernet Protocol):

* Activate/Release a playback
* Set the level of a playback
* Hit go on playback
* Stop and/or back on playback
* Jump to a specific cue ID on a playback

## Building

This plugin uses CMake as its build system. You will need at least version 3.12
of CMake to build. You will need the following library depedencies (along with
their development packages) to compile this plugin:

* cmake
* gcc/g++
* pkg-config
* gtk3
* glib2
* jsoncpp
* [Stack](https://github.com/claytonpeters/stack) itself

The CMake scripts assumes you have a clone of the Stack Git repository at
`../stack`, relative to the checkout of this plugin. If you've got it elsewhere
checked out, change `CMakeLists.txt` and alter the line:

```cmake
include_directories("${PROJECT_SOURCE_DIR}/../stack/src")
```

Change this to point to the `src` directory inside your clone of the Stack
repo.

Compilation (once you have the correct depedencies installed), should be as
simple as:

```shell
cmake .
make
```

This will cmpile the plugin. To use the plugin with Stack, copy the compiled
library `libStackMagicQCue.so` to the directory containing the other Stack
plugins (which is usually the same directory as the `runstack` binary).

Once ready, the plugin will appear as a **MagicQ Action** cue type in the Stack
UI.

## Configuration

### MagicQ

MagicQ must be configured to receive OpenShowControl (OSC) commands. This is
configured under Setup, under the networking tab. The relevant settings are:

* **OSC mode**: Must be set to `Tx` or `Tx and Rx`
* **OSC rx port:** Whatever UDP port you want to use. Default is `8000`

### Stack

Currently, whilst awaiting changes to Stack for application-wide settings for
plugins, the cue is hardcoded to talk to MagicQ on the local host (i.e. at IP
address 127.0.0.1). The UDP port used for OSC defaults to `8000`, which is the
default in MagicQ. This, however, is customisable by setting the
`STACK_MAGICQ_OSC_PORT` environment variable (again, whilst waiting for a change
to Stack to allo for a UI).


