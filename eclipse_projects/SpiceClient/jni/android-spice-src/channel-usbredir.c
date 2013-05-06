/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright 2010-2011 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>
   Richard Hughes <rhughes@redhat.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "config.h"

#ifdef USE_USBREDIR
#include <usbredirhost.h>
#if USE_POLKIT
#include "usb-acl-helper.h"
#endif
#include "channel-usbredir-priv.h"
#include "usb-device-manager-priv.h"
#endif

#include "spice-client.h"
#include "spice-common.h"

#include "spice-channel-priv.h"
#include "glib-compat.h"

/**
 * SECTION:channel-usbredir
 * @short_description: usb redirection
 * @title: USB Redirection Channel
 * @section_id:
 * @stability: API Stable (channel in development)
 * @include: channel-usbredir.h
 *
 * The Spice protocol defines a set of messages to redirect USB devices
 * from the Spice client to the VM. This channel handles these messages.
 */

#ifdef USE_USBREDIR

#define SPICE_USBREDIR_CHANNEL_GET_PRIVATE(obj)                                  \
    (G_TYPE_INSTANCE_GET_PRIVATE((obj), SPICE_TYPE_USBREDIR_CHANNEL, SpiceUsbredirChannelPrivate))

enum SpiceUsbredirChannelState {
    STATE_DISCONNECTED,
#if USE_POLKIT
    STATE_WAITING_FOR_ACL_HELPER,
#endif
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_DISCONNECTING,
};

struct _SpiceUsbredirChannelPrivate {
    libusb_context *context;
    libusb_device *device;
    struct usbredirhost *host;
    /* To catch usbredirhost error messages and report them as a GError */
    GError **catch_error;
    /* Data passed from channel handle msg to the usbredirhost read cb */
    const uint8_t *read_buf;
    int read_buf_size;
    enum SpiceUsbredirChannelState state;
#if USE_POLKIT
    GSimpleAsyncResult *result;
    SpiceUsbAclHelper *acl_helper;
#endif
};

static void spice_usbredir_handle_msg(SpiceChannel *channel, SpiceMsgIn *msg);
static void spice_usbredir_channel_up(SpiceChannel *channel);
static void spice_usbredir_channel_dispose(GObject *obj);
static void usbredir_handle_msg(SpiceChannel *channel, SpiceMsgIn *in);

static void usbredir_log(void *user_data, int level, const char *msg);
static int usbredir_read_callback(void *user_data, uint8_t *data, int count);
static int usbredir_write_callback(void *user_data, uint8_t *data, int count);
static void usbredir_write_flush_callback(void *user_data);

static void *usbredir_alloc_lock(void);
static void usbredir_lock_lock(void *user_data);
static void usbredir_unlock_lock(void *user_data);
static void usbredir_free_lock(void *user_data);

#endif

G_DEFINE_TYPE(SpiceUsbredirChannel, spice_usbredir_channel, SPICE_TYPE_CHANNEL)

/* ------------------------------------------------------------------ */

static void spice_usbredir_channel_init(SpiceUsbredirChannel *channel)
{
#ifdef USE_USBREDIR
    channel->priv = SPICE_USBREDIR_CHANNEL_GET_PRIVATE(channel);
#endif
}

#ifdef USE_USBREDIR
static void spice_usbredir_channel_reset(SpiceChannel *channel, gboolean migrating)
{
    SPICE_CHANNEL_CLASS(spice_usbredir_channel_parent_class)->channel_reset(channel, migrating);
}
#endif

static void spice_usbredir_channel_class_init(SpiceUsbredirChannelClass *klass)
{
#ifdef USE_USBREDIR
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    SpiceChannelClass *channel_class = SPICE_CHANNEL_CLASS(klass);

    gobject_class->dispose      = spice_usbredir_channel_dispose;
    channel_class->handle_msg   = spice_usbredir_handle_msg;
    channel_class->channel_up   = spice_usbredir_channel_up;
    channel_class->channel_reset = spice_usbredir_channel_reset;

    g_type_class_add_private(klass, sizeof(SpiceUsbredirChannelPrivate));
#endif
}

#ifdef USE_USBREDIR
static void spice_usbredir_channel_dispose(GObject *obj)
{
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(obj);

    spice_usbredir_channel_disconnect(channel);

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_usbredir_channel_parent_class)->dispose)
        G_OBJECT_CLASS(spice_usbredir_channel_parent_class)->dispose(obj);
}

/*
 * Note we don't have a finalize to unref our : device / context / acl_helper /
 * result references. The reason for this is that depending on our state they
 * are either:
 * 1) Already unreferenced
 * 2) Will be unreferenced by the disconnect call from dispose
 * 3) Will be unreferenced by spice_usbredir_channel_open_acl_cb
 *
 * Now the last one may seem like an issue, since what will happen if
 * spice_usbredir_channel_open_acl_cb will run after finalization?
 *
 * This will never happens since the GSimpleAsyncResult created before we
 * get into the STATE_WAITING_FOR_ACL_HELPER takes a reference to its
 * source object, which is our SpiceUsbredirChannel object, so
 * the finalize won't hapen until spice_usbredir_channel_open_acl_cb runs,
 * and unrefs priv->result which will in turn unref ourselve once the
 * complete_in_idle call it does has completed. And once
 * spice_usbredir_channel_open_acl_cb has run, all references we hold have
 * been released even in the 3th scenario.
 */

static const spice_msg_handler usbredir_handlers[] = {
    [ SPICE_MSG_SPICEVMC_DATA ] = usbredir_handle_msg,
};

/* ------------------------------------------------------------------ */
/* private api                                                        */

static gboolean spice_usbredir_channel_open_device(
    SpiceUsbredirChannel *channel, GError **err)
{
    SpiceUsbredirChannelPrivate *priv = channel->priv;
    libusb_device_handle *handle = NULL;
    int rc;

    g_return_val_if_fail(priv->state == STATE_DISCONNECTED
#if USE_POLKIT
                         || priv->state == STATE_WAITING_FOR_ACL_HELPER
#endif
                         , FALSE);

    rc = libusb_open(priv->device, &handle);
    if (rc != 0) {
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    "Could not open usb device: %s [%i]",
                    spice_usb_device_manager_libusb_strerror(rc), rc);
        return FALSE;
    }

    priv->catch_error = err;
    priv->host = usbredirhost_open_full(
                                   priv->context,
                                   handle, usbredir_log,
                                   usbredir_read_callback,
                                   usbredir_write_callback,
                                   usbredir_write_flush_callback,
                                   usbredir_alloc_lock,
                                   usbredir_lock_lock,
                                   usbredir_unlock_lock,
                                   usbredir_free_lock,
                                   channel, PACKAGE_STRING,
                                   spice_util_get_debug() ? usbredirparser_debug : usbredirparser_warning,
                                   usbredirhost_fl_write_cb_owns_buffer);
    priv->catch_error = NULL;
    if (!priv->host) {
        g_return_val_if_fail(err == NULL || *err != NULL, FALSE);
        return FALSE;
    }

    if (!spice_usb_device_manager_start_event_listening(
            spice_usb_device_manager_get(
                spice_channel_get_session(SPICE_CHANNEL(channel)), NULL),
            err)) {
        usbredirhost_close(priv->host);
        priv->host = NULL;
        return FALSE;
    }

    spice_channel_connect(SPICE_CHANNEL(channel));
    priv->state = STATE_CONNECTING;

    return TRUE;
}

#if USE_POLKIT
static void spice_usbredir_channel_open_acl_cb(
    GObject *gobject, GAsyncResult *acl_res, gpointer user_data)
{
    SpiceUsbAclHelper *acl_helper = SPICE_USB_ACL_HELPER(gobject);
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(user_data);
    SpiceUsbredirChannelPrivate *priv = channel->priv;
    GError *err = NULL;

    g_return_if_fail(acl_helper == priv->acl_helper);
    g_return_if_fail(priv->state == STATE_WAITING_FOR_ACL_HELPER ||
                     priv->state == STATE_DISCONNECTING);

    spice_usb_acl_helper_open_acl_finish(acl_helper, acl_res, &err);
    if (!err && priv->state == STATE_DISCONNECTING) {
        err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                  "USB redirection channel connect cancelled");
    }
    if (!err) {
        spice_usbredir_channel_open_device(channel, &err);
    }
    if (err) {
        g_simple_async_result_take_error(priv->result, err);
        libusb_unref_device(priv->device);
        priv->device  = NULL;
        priv->context = NULL;
        priv->state = STATE_DISCONNECTED;
    }

    spice_usb_acl_helper_close_acl(priv->acl_helper);
    g_clear_object(&priv->acl_helper);
    g_object_set(spice_channel_get_session(SPICE_CHANNEL(channel)),
                 "inhibit-keyboard-grab", FALSE, NULL);

    g_simple_async_result_complete_in_idle(priv->result);
    g_clear_object(&priv->result);
}
#endif

G_GNUC_INTERNAL
void spice_usbredir_channel_connect_async(SpiceUsbredirChannel *channel,
                                          libusb_context       *context,
                                          libusb_device        *device,
                                          GCancellable         *cancellable,
                                          GAsyncReadyCallback   callback,
                                          gpointer              user_data)
{
    SpiceUsbredirChannelPrivate *priv = channel->priv;
    GSimpleAsyncResult *result;

    g_return_if_fail(SPICE_IS_USBREDIR_CHANNEL(channel));
    g_return_if_fail(context != NULL);
    g_return_if_fail(device != NULL);

    SPICE_DEBUG("connecting usb channel %p", channel);

    result = g_simple_async_result_new(G_OBJECT(channel), callback, user_data,
                                       spice_usbredir_channel_connect_async);

    if (priv->state != STATE_DISCONNECTED) {
        g_simple_async_result_set_error(result,
                            SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            "Error channel is busy");
        goto done;
    }

    priv->context = context;
    priv->device  = libusb_ref_device(device);
#if USE_POLKIT
    priv->result = result;
    priv->state = STATE_WAITING_FOR_ACL_HELPER;
    priv->acl_helper = spice_usb_acl_helper_new();
    g_object_set(spice_channel_get_session(SPICE_CHANNEL(channel)),
                 "inhibit-keyboard-grab", TRUE, NULL);
    spice_usb_acl_helper_open_acl(priv->acl_helper,
                                  libusb_get_bus_number(device),
                                  libusb_get_device_address(device),
                                  cancellable,
                                  spice_usbredir_channel_open_acl_cb,
                                  channel);
    return;
#else
    GError *err = NULL;
    if (!spice_usbredir_channel_open_device(channel, &err)) {
        g_simple_async_result_take_error(result, err);
        libusb_unref_device(priv->device);
        priv->device  = NULL;
        priv->context = NULL;
    }
#endif

done:
    g_simple_async_result_complete_in_idle(result);
    g_object_unref(result);
}

G_GNUC_INTERNAL
gboolean spice_usbredir_channel_connect_finish(SpiceUsbredirChannel *channel,
                                               GAsyncResult         *res,
                                               GError              **err)
{
    GSimpleAsyncResult *result = G_SIMPLE_ASYNC_RESULT(res);

    g_return_val_if_fail(g_simple_async_result_is_valid(res, G_OBJECT(channel),
                                     spice_usbredir_channel_connect_async),
                         FALSE);

    if (g_simple_async_result_propagate_error(result, err))
        return FALSE;

    return TRUE;
}

G_GNUC_INTERNAL
void spice_usbredir_channel_disconnect(SpiceUsbredirChannel *channel)
{
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    SPICE_DEBUG("disconnecting usb channel %p", channel);

    switch (priv->state) {
    case STATE_DISCONNECTED:
    case STATE_DISCONNECTING:
        break;
#if USE_POLKIT
    case STATE_WAITING_FOR_ACL_HELPER:
        priv->state = STATE_DISCONNECTING;
        /* We're still waiting for the acl helper -> cancel it */
        spice_usb_acl_helper_close_acl(priv->acl_helper);
        break;
#endif
    case STATE_CONNECTING:
    case STATE_CONNECTED:
        spice_channel_disconnect(SPICE_CHANNEL(channel), SPICE_CHANNEL_NONE);
        /*
         * This sets the usb event thread run condition to FALSE, therefor
         * it must be done before usbredirhost_close, as usbredirhost_close
         * will interrupt the libusb_handle_events call in the thread.
         */
        spice_usb_device_manager_stop_event_listening(
            spice_usb_device_manager_get(
                spice_channel_get_session(SPICE_CHANNEL(channel)), NULL));
        /* This also closes the libusb handle we passed to its _open */
        usbredirhost_close(priv->host);
        priv->host = NULL;
        libusb_unref_device(priv->device);
        priv->device  = NULL;
        priv->context = NULL;
        priv->state = STATE_DISCONNECTED;
        break;
    }
}

G_GNUC_INTERNAL
libusb_device *spice_usbredir_channel_get_device(SpiceUsbredirChannel *channel)
{
    return channel->priv->device;
}

/* Note that this function must be re-entrant safe, as it can get called
   from both the main thread as well as from the usb event handling thread */
static void usbredir_write_flush_callback(void *user_data)
{
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(user_data);
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    if (priv->state != STATE_CONNECTED)
        return;

    usbredirhost_write_guest_data(priv->host);
}

/* ------------------------------------------------------------------ */
/* callbacks (any context)                                            */

static void usbredir_log(void *user_data, int level, const char *msg)
{
    SpiceUsbredirChannel *channel = user_data;
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    if (priv->catch_error && level == usbredirparser_error) {
        SPICE_DEBUG("%s", msg);
        g_set_error_literal(priv->catch_error, SPICE_CLIENT_ERROR,
                            SPICE_CLIENT_ERROR_FAILED, msg);
        return;
    }

    switch (level) {
        case usbredirparser_error:
            g_critical("%s", msg); break;
        case usbredirparser_warning:
            g_warning("%s", msg); break;
        default:
            SPICE_DEBUG("%s", msg); break;
    }
}

static int usbredir_read_callback(void *user_data, uint8_t *data, int count)
{
    SpiceUsbredirChannel *channel = user_data;
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    if (priv->read_buf_size < count) {
        count = priv->read_buf_size;
    }

    memcpy(data, priv->read_buf, count);

    priv->read_buf_size -= count;
    if (priv->read_buf_size) {
        priv->read_buf += count;
    } else {
        priv->read_buf = NULL;
    }

    return count;
}

static void usbredir_free_write_cb_data(uint8_t *data, void *user_data)
{
    SpiceUsbredirChannel *channel = user_data;
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    usbredirhost_free_write_buffer(priv->host, data);
}

static int usbredir_write_callback(void *user_data, uint8_t *data, int count)
{
    SpiceUsbredirChannel *channel = user_data;
    SpiceMsgOut *msg_out;

    msg_out = spice_msg_out_new(SPICE_CHANNEL(channel),
                                SPICE_MSGC_SPICEVMC_DATA);
    spice_marshaller_add_ref_full(msg_out->marshaller, data, count,
                                  usbredir_free_write_cb_data, channel);
    spice_msg_out_send(msg_out);

    return count;
}

static void *usbredir_alloc_lock(void) {
    return g_mutex_new();
}

static void usbredir_lock_lock(void *user_data) {
    GMutex *mutex = user_data;

    g_mutex_lock(mutex);
}

static void usbredir_unlock_lock(void *user_data) {
    GMutex *mutex = user_data;

    g_mutex_unlock(mutex);
}

static void usbredir_free_lock(void *user_data) {
    GMutex *mutex = user_data;

    g_mutex_free(mutex);
}

/* --------------------------------------------------------------------- */
/* coroutine context                                                     */
static void spice_usbredir_handle_msg(SpiceChannel *c, SpiceMsgIn *msg)
{
    int type = spice_msg_in_type(msg);
    SpiceChannelClass *parent_class;

    g_return_if_fail(type < SPICE_N_ELEMENTS(usbredir_handlers));

    parent_class = SPICE_CHANNEL_CLASS(spice_usbredir_channel_parent_class);

    if (usbredir_handlers[type] != NULL)
        usbredir_handlers[type](c, msg);
    else if (parent_class->handle_msg)
        parent_class->handle_msg(c, msg);
    else
        g_return_if_reached();
}

static void spice_usbredir_channel_up(SpiceChannel *c)
{
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(c);
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    g_return_if_fail(priv->state == STATE_CONNECTING);

    priv->state = STATE_CONNECTED;
    /* Flush any pending writes */
    usbredirhost_write_guest_data(priv->host);
}

static void usbredir_handle_msg(SpiceChannel *c, SpiceMsgIn *in)
{
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(c);
    SpiceUsbredirChannelPrivate *priv = channel->priv;
    int size;
    uint8_t *buf;

    g_return_if_fail(priv->host != NULL);

    /* No recursion allowed! */
    g_return_if_fail(priv->read_buf == NULL);

    buf = spice_msg_in_raw(in, &size);
    priv->read_buf = buf;
    priv->read_buf_size = size;

    usbredirhost_read_guest_data(priv->host);
}

#endif /* USE_USBREDIR */
