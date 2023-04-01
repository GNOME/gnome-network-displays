#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define ND_TYPE_FIREWALLD (nd_firewalld_get_type ())

/* Can only contain numbers, characters and '_', '-', '/'. Also limited to
 * 17 characters.
 * Keep in sync with installable file name.
 */
#define ND_WFD_ZONE "P2P-WiFi-Display"

G_DECLARE_FINAL_TYPE (NdFirewalld, nd_firewalld, ND, FIREWALLD, GObject)

NdFirewalld *nd_firewalld_new (void);

void     nd_firewalld_ensure_wfd_zone (NdFirewalld        *self,
                                       GCancellable       *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer            user_data);
gboolean nd_firewalld_ensure_wfd_zone_finish (NdFirewalld  *self,
                                              GAsyncResult *res,
                                              GError      **error);
G_END_DECLS
