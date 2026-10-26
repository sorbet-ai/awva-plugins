// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// A plugin module (the .clap, and the VST3's and AU's own binaries): the implementation library's
// clap_entry, kept by the linker (a module whose only object references it).
#include <clap/clap.h>

extern "C" const clap_plugin_entry clap_entry;
extern "C" CLAP_EXPORT const void* const awva_plugin_keep_entry = &clap_entry;
