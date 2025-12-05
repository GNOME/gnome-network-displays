/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <glib-object.h>
#include <NetworkManager.h>
#include "nd-provider.h"
#include "nd-cc-sink.h"

G_BEGIN_DECLS

#define ND_TYPE_SD_CC_PROVIDER (nd_sd_cc_provider_get_type ())
G_DECLARE_FINAL_TYPE (NdSdCCProvider, nd_sd_cc_provider, ND, SD_CC_PROVIDER, GObject)

/* Construct a new systemd-resolved-based Chromecast provider */
NdSdCCProvider *nd_sd_cc_provider_new (NMDevice *nm_device);

/* Start browsing for Chromecast (_googlecast._tcp) services */
gboolean nd_sd_cc_provider_browse (NdSdCCProvider *provider,
                                   GError        **error);

GSocketClient *nd_sd_cc_provider_get_signalling_client (NdSdCCProvider *provider);

G_END_DECLS
