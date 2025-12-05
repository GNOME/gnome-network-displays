/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <glib-object.h>
#include <NetworkManager.h>
#include "nd-provider.h"
#include "nd-wfd-mice-sink.h"

G_BEGIN_DECLS

#define ND_TYPE_SD_WFD_MICE_PROVIDER (nd_sd_wfd_mice_provider_get_type())
G_DECLARE_FINAL_TYPE (NdSdWfdMiceProvider, nd_sd_wfd_mice_provider, ND, SD_WFD_MICE_PROVIDER, GObject)

/* Construct a new systemd-resolved-based MICE provider */
NdSdWfdMiceProvider *nd_sd_wfd_mice_provider_new (NMDevice *nm_device);

/* Start browsing for MICE (_display._tcp) services */
gboolean nd_sd_wfd_mice_provider_browse (NdSdWfdMiceProvider *provider,
                                         GError             **error);

G_END_DECLS
