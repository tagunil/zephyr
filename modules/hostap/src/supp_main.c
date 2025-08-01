/*
 * Copyright (c) 2023 Nordic Semiconductor ASA.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wifi_supplicant, CONFIG_WIFI_NM_WPA_SUPPLICANT_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <poll.h>
#include <zephyr/zvfs/eventfd.h>

#if !defined(CONFIG_WIFI_NM_WPA_SUPPLICANT_CRYPTO_NONE) && !defined(CONFIG_MBEDTLS_ENABLE_HEAP)
#include <mbedtls/platform.h>
#endif /* !CONFIG_WIFI_NM_WPA_SUPPLICANT_CRYPTO_NONE && !CONFIG_MBEDTLS_ENABLE_HEAP */
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_CRYPTO_MBEDTLS_PSA
#include "supp_psa_api.h"
#endif

#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wifi_nm.h>
#include <zephyr/net/socket.h>

static K_THREAD_STACK_DEFINE(supplicant_thread_stack,
			     CONFIG_WIFI_NM_WPA_SUPPLICANT_THREAD_STACK_SIZE);
static struct k_thread tid;

static K_THREAD_STACK_DEFINE(iface_wq_stack, CONFIG_WIFI_NM_WPA_SUPPLICANT_WQ_STACK_SIZE);

#define IFACE_NOTIFY_TIMEOUT_MS 1000
#define IFACE_NOTIFY_RETRY_MS 10

#include "supp_main.h"
#include "supp_api.h"
#include "supp_events.h"

#include "includes.h"
#include "common.h"
#include "eloop.h"
#include "wpa_supplicant/config.h"
#include "wpa_supplicant_i.h"
#include "fst/fst.h"
#include "includes.h"
#include "wpa_cli_zephyr.h"
#include "ctrl_iface_zephyr.h"
#ifdef CONFIG_WIFI_NM_HOSTAPD_AP
#include "hostapd.h"
#include "hapd_main.h"
#endif

static const struct wifi_mgmt_ops mgmt_ops = {
	.get_version = supplicant_get_version,
	.scan = supplicant_scan,
	.connect = supplicant_connect,
	.disconnect = supplicant_disconnect,
	.iface_status = supplicant_status,
#ifdef CONFIG_NET_STATISTICS_WIFI
	.get_stats = supplicant_get_stats,
	.reset_stats = supplicant_reset_stats,
#endif
	.cfg_11k = supplicant_11k_cfg,
	.send_11k_neighbor_request = supplicant_11k_neighbor_request,
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_ROAMING
	.candidate_scan = supplicant_candidate_scan,
	.start_11r_roaming = supplicant_11r_roaming,
#endif
	.set_power_save = supplicant_set_power_save,
	.set_twt = supplicant_set_twt,
	.get_power_save_config = supplicant_get_power_save_config,
	.reg_domain = supplicant_reg_domain,
	.mode = supplicant_mode,
	.filter = supplicant_filter,
	.channel = supplicant_channel,
	.set_rts_threshold = supplicant_set_rts_threshold,
	.get_rts_threshold = supplicant_get_rts_threshold,
	.bss_support_neighbor_rep = supplicant_bss_support_neighbor_rep,
	.bss_ext_capab = supplicant_bss_ext_capab,
	.legacy_roam = supplicant_legacy_roam,
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_WNM
	.btm_query = supplicant_btm_query,
#endif
	.get_conn_params = supplicant_get_wifi_conn_params,
	.wps_config = supplicant_wps_config,
	.set_bss_max_idle_period = supplicant_set_bss_max_idle_period,
#ifdef CONFIG_AP
	.ap_enable = supplicant_ap_enable,
	.ap_disable = supplicant_ap_disable,
	.ap_sta_disconnect = supplicant_ap_sta_disconnect,
#endif /* CONFIG_AP */
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_DPP
	.dpp_dispatch = supplicant_dpp_dispatch,
#endif /* CONFIG_WIFI_NM_WPA_SUPPLICANT_DPP */
	.pmksa_flush = supplicant_pmksa_flush,
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_CRYPTO_ENTERPRISE
	.enterprise_creds = supplicant_add_enterprise_creds,
#endif
	.config_params = supplicant_config_params,
};

DEFINE_WIFI_NM_INSTANCE(wifi_supplicant, &mgmt_ops);

#define WRITE_TIMEOUT 100 /* ms */
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_INF_MON
#define INTERFACE_EVENT_MASK (NET_EVENT_IF_ADMIN_UP | NET_EVENT_IF_ADMIN_DOWN)
#endif
struct supplicant_context {
	struct wpa_global *supplicant;
#ifdef CONFIG_WIFI_NM_HOSTAPD_AP
	struct hapd_interfaces hostapd;
#endif
	struct net_mgmt_event_callback cb;
	struct net_if *iface;
	char if_name[CONFIG_NET_INTERFACE_NAME_LEN + 1];
	struct k_fifo fifo;
	int sock;
	struct k_work iface_work;
	struct k_work_q iface_wq;
	int (*iface_handler)(struct supplicant_context *ctx, struct net_if *iface);
};

static struct supplicant_context *get_default_context(void)
{
	static struct supplicant_context ctx;

	return &ctx;
}

struct wpa_global *zephyr_get_default_supplicant_context(void)
{
	return get_default_context()->supplicant;
}

#ifdef CONFIG_WIFI_NM_HOSTAPD_AP
struct hapd_interfaces *zephyr_get_default_hapd_context(void)
{
	return &get_default_context()->hostapd;
}
#endif

struct k_work_q *get_workq(void)
{
	return &get_default_context()->iface_wq;
}

/* found in hostap/wpa_supplicant/ctrl_iface_zephyr.c */
extern int send_data(struct k_fifo *fifo, int sock, const char *buf, size_t len, int flags);

int zephyr_wifi_send_event(const struct wpa_supplicant_event_msg *msg)
{
	struct supplicant_context *ctx;
	int ret;

	/* TODO: Fix this to get the correct container */
	ctx = get_default_context();

	if (ctx->sock < 0) {
		ret = -ENOENT;
		goto out;
	}

	ret = send_data(&ctx->fifo, ctx->sock,
			(const char *)msg, sizeof(*msg), 0);
	if (ret != 0) {
		ret = -EMSGSIZE;
		LOG_WRN("Event partial send (%d)", ret);
		goto out;
	}

	ret = 0;

out:
	return ret;
}

#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_INF_MON
static int send_event(const struct wpa_supplicant_event_msg *msg)
{
	return zephyr_wifi_send_event(msg);
}

static bool is_wanted_interface(struct net_if *iface)
{
	if (!net_if_is_wifi(iface)) {
		return false;
	}

	/* TODO: check against a list of valid interfaces */

	return true;
}
#endif
struct wpa_supplicant *zephyr_get_handle_by_ifname(const char *ifname)
{
	struct wpa_supplicant *wpa_s = NULL;
	struct supplicant_context *ctx = get_default_context();

	wpa_s = wpa_supplicant_get_iface(ctx->supplicant, ifname);
	if (!wpa_s) {
		wpa_printf(MSG_ERROR, "%s: Unable to get wpa_s handle for %s\n", __func__, ifname);
		return NULL;
	}

	return wpa_s;
}

static int get_iface_count(struct supplicant_context *ctx)
{
	/* FIXME, should not access ifaces as it is supplicant internal data */
	struct wpa_supplicant *wpa_s;
	unsigned int count = 0;

	for (wpa_s = ctx->supplicant->ifaces; wpa_s; wpa_s = wpa_s->next) {
		count += 1;
	}

	return count;
}

static void zephyr_wpa_supplicant_msg(void *ctx, const char *txt, size_t len)
{
	struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)ctx;

	if (!ctx || !txt) {
		return;
	}

	/* Only interested in CTRL-EVENTs */
	if (strncmp(txt, "CTRL-EVENT", 10) == 0) {
		if (strncmp(txt, "CTRL-EVENT-SIGNAL-CHANGE", 24) == 0) {
			supplicant_send_wifi_mgmt_event(wpa_s->ifname,
						NET_EVENT_WIFI_CMD_SIGNAL_CHANGE,
						(void *)txt, len);
		} else {
			supplicant_send_wifi_mgmt_event(wpa_s->ifname,
						NET_EVENT_WIFI_CMD_SUPPLICANT,
						(void *)txt, len);
		}
	} else if (strncmp(txt, "RRM-NEIGHBOR-REP-RECEIVED", 25) == 0) {
		supplicant_send_wifi_mgmt_event(wpa_s->ifname,
						NET_EVENT_WIFI_CMD_NEIGHBOR_REP_RECEIVED,
						(void *)txt, len);
	}
}

static const char *zephyr_hostap_msg_ifname_cb(void *ctx)
{
	if (ctx == NULL) {
		return NULL;
	}

#ifdef CONFIG_WIFI_NM_HOSTAPD_AP
	if ((((struct wpa_supplicant *)ctx))->is_hostapd == 0) {
		struct wpa_supplicant *wpa_s = ctx;

		return wpa_s->ifname;
	}

	struct hostapd_data *hapd = ctx;

	if (hapd && hapd->conf) {
		return hapd->conf->iface;
	}

	return NULL;
#else
	struct wpa_supplicant *wpa_s = ctx;

	return wpa_s->ifname;
#endif
}

static void zephyr_hostap_ctrl_iface_msg_cb(void *ctx, int level, enum wpa_msg_type type,
					    const char *txt, size_t len)
{
	ARG_UNUSED(level);
	ARG_UNUSED(type);

	if (ctx == NULL) {
		return;
	}

#ifdef CONFIG_WIFI_NM_HOSTAPD_AP
	if ((((struct wpa_supplicant *)ctx))->is_hostapd == 0) {
		zephyr_wpa_supplicant_msg(ctx, txt, len);
	} else {
		zephyr_hostapd_msg(ctx, txt, len);
	}
#else
	zephyr_wpa_supplicant_msg(ctx, txt, len);
#endif
}

static int add_interface(struct supplicant_context *ctx, struct net_if *iface)
{
	struct wpa_supplicant *wpa_s;
	char ifname[IFNAMSIZ + 1] = { 0 };
	int ret, retry = 0, count = IFACE_NOTIFY_TIMEOUT_MS / IFACE_NOTIFY_RETRY_MS;

	ret = net_if_get_name(iface, ifname, sizeof(ifname) - 1);
	if (ret < 0) {
		LOG_ERR("Cannot get interface %d (%p) name", net_if_get_by_iface(iface), iface);
		goto out;
	}

	LOG_DBG("Adding interface %s [%d] (%p)", ifname, net_if_get_by_iface(iface), iface);

	ret = zephyr_wpa_cli_global_cmd_v("interface_add %s %s %s %s",
					  ifname, "zephyr", "zephyr", "zephyr");
	if (ret) {
		LOG_ERR("Failed to add interface %s", ifname);
		goto out;
	}

	while (retry++ < count && !wpa_supplicant_get_iface(ctx->supplicant, ifname)) {
		k_sleep(K_MSEC(IFACE_NOTIFY_RETRY_MS));
	}

	wpa_s = wpa_supplicant_get_iface(ctx->supplicant, ifname);
	if (wpa_s == NULL) {
		LOG_ERR("Failed to add iface %s", ifname);
		goto out;
	}

	wpa_s->conf->filter_ssids = 1;
	wpa_s->conf->ap_scan = 1;

	/* Default interface, kick start supplicant */
	if (get_iface_count(ctx) > 0) {
		ctx->iface = iface;
		net_if_get_name(iface, ctx->if_name, CONFIG_NET_INTERFACE_NAME_LEN);
	}

	ret = zephyr_wpa_ctrl_init(wpa_s);
	if (ret) {
		LOG_ERR("Failed to initialize supplicant control interface");
		goto out;
	}

	ret = wifi_nm_register_mgd_type_iface(wifi_nm_get_instance("wifi_supplicant"),
					      WIFI_TYPE_STA,
					      iface);
	if (ret) {
		LOG_ERR("Failed to register mgd iface with native stack %s (%d)",
			ifname, ret);
		goto out;
	}

	supplicant_generate_state_event(ifname, NET_EVENT_SUPPLICANT_CMD_IFACE_ADDED, 0);

	if (get_iface_count(ctx) == 1) {
		supplicant_generate_state_event(ifname, NET_EVENT_SUPPLICANT_CMD_READY, 0);
	}

	wpa_msg_register_cb(zephyr_hostap_ctrl_iface_msg_cb);
	ret = 0;

out:
	return ret;
}
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_INF_MON
static int del_interface(struct supplicant_context *ctx, struct net_if *iface)
{
	struct wpa_supplicant_event_msg msg;
	struct wpa_supplicant *wpa_s;
	union wpa_event_data *event = NULL;
	int ret, retry = 0, count = IFACE_NOTIFY_TIMEOUT_MS / IFACE_NOTIFY_RETRY_MS;
	char ifname[IFNAMSIZ + 1] = { 0 };

	ret = net_if_get_name(iface, ifname, sizeof(ifname) - 1);
	if (ret < 0) {
		LOG_ERR("Cannot get interface %d (%p) name", net_if_get_by_iface(iface), iface);
		goto out;
	}

	LOG_DBG("Removing interface %s %d (%p)", ifname, net_if_get_by_iface(iface), iface);

	event = os_zalloc(sizeof(*event));
	if (!event) {
		ret = -ENOMEM;
		LOG_ERR("Failed to allocate event data");
		goto out;
	}

	wpa_s = wpa_supplicant_get_iface(ctx->supplicant, ifname);
	if (!wpa_s) {
		ret = -ENOENT;
		LOG_ERR("Failed to get wpa_s handle for %s", ifname);
		goto free;
	}

	supplicant_generate_state_event(ifname, NET_EVENT_SUPPLICANT_CMD_IFACE_REMOVING, 0);

	if (sizeof(event->interface_status.ifname) < strlen(ifname)) {
		wpa_printf(MSG_ERROR, "Interface name too long: %s (max: %zu)",
			ifname, sizeof(event->interface_status.ifname));
		goto free;
	}

	os_memcpy(event->interface_status.ifname, ifname, strlen(ifname));
	event->interface_status.ievent = EVENT_INTERFACE_REMOVED;

	msg.global = true;
	msg.ctx = ctx->supplicant;
	msg.event = EVENT_INTERFACE_STATUS;
	msg.data = event;

	ret = send_event(&msg);
	if (ret) {
		/* We failed notify WPA supplicant about interface removal.
		 * There is not much we can do, interface is still registered
		 * with WPA supplicant so we cannot unregister NM etc.
		 */
		wpa_printf(MSG_ERROR, "Failed to send event: %d", ret);
		goto free;
	}

	while (retry++ < count && wpa_s->wpa_state != WPA_INTERFACE_DISABLED) {
		k_sleep(K_MSEC(IFACE_NOTIFY_RETRY_MS));
	}

	if (wpa_s->wpa_state != WPA_INTERFACE_DISABLED) {
		LOG_ERR("Failed to notify remove interface %s", ifname);
		supplicant_generate_state_event(ifname, NET_EVENT_SUPPLICANT_CMD_IFACE_REMOVED, -1);
		goto out;
	}

	zephyr_wpa_ctrl_deinit(wpa_s);

	ret = zephyr_wpa_cli_global_cmd_v("interface_remove %s", ifname);
	if (ret) {
		LOG_ERR("Failed to remove interface %s", ifname);
		supplicant_generate_state_event(ifname, NET_EVENT_SUPPLICANT_CMD_IFACE_REMOVED,
					  -EINVAL);
		goto out;
	}

	ret = wifi_nm_unregister_mgd_iface(wifi_nm_get_instance("wifi_supplicant"), iface);
	if (ret) {
		LOG_ERR("Failed to unregister mgd iface %s with native stack (%d)",
			ifname, ret);
		goto out;
	}

	if (get_iface_count(ctx) == 0) {
		supplicant_generate_state_event(ifname, NET_EVENT_SUPPLICANT_CMD_NOT_READY, 0);
	}

	supplicant_generate_state_event(ifname, NET_EVENT_SUPPLICANT_CMD_IFACE_REMOVED, 0);

	return 0;

free:
	if (event) {
		os_free(event);
	}
out:
	return ret;
}
#endif
static void iface_work_handler(struct k_work *work)
{
	struct supplicant_context *ctx = CONTAINER_OF(work, struct supplicant_context,
						      iface_work);
	int ret;

	ret = (*ctx->iface_handler)(ctx, ctx->iface);
	if (ret < 0) {
		LOG_ERR("Interface %d (%p) handler failed (%d)",
			net_if_get_by_iface(ctx->iface), ctx->iface, ret);
	}
}

/* As the mgmt thread stack is limited, use a separate work queue for any network
 * interface add/delete.
 */
static void submit_iface_work(struct supplicant_context *ctx,
			      struct net_if *iface,
			      int (*handler)(struct supplicant_context *ctx,
					     struct net_if *iface))
{
	ctx->iface_handler = handler;

	k_work_submit_to_queue(&ctx->iface_wq, &ctx->iface_work);
}
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_INF_MON
static void interface_handler(struct net_mgmt_event_callback *cb,
			      uint64_t mgmt_event, struct net_if *iface)
{
	if ((mgmt_event & INTERFACE_EVENT_MASK) != mgmt_event) {
		return;
	}

	if (!is_wanted_interface(iface)) {
		LOG_DBG("Ignoring event (0x%" PRIx64 ") from interface %d (%p)",
			mgmt_event, net_if_get_by_iface(iface), iface);
		return;
	}

	if (mgmt_event == NET_EVENT_IF_ADMIN_UP) {
		LOG_INF("Network interface %d (%p) up", net_if_get_by_iface(iface), iface);
		add_interface(get_default_context(), iface);
		return;
	}

	if (mgmt_event == NET_EVENT_IF_ADMIN_DOWN) {
		LOG_INF("Network interface %d (%p) down", net_if_get_by_iface(iface), iface);
		del_interface(get_default_context(), iface);
		return;
	}
}
#endif

static void iface_cb(struct net_if *iface, void *user_data)
{
	struct supplicant_context *ctx = user_data;
	int ret;

	if (!net_if_is_wifi(iface)) {
		return;
	}

#ifdef CONFIG_WIFI_NM_HOSTAPD_AP
	if (wifi_nm_iface_is_sap(iface)) {
		return;
	}
#endif

	if (!net_if_is_admin_up(iface)) {
		return;
	}

	ret = add_interface(ctx, iface);
	if (ret < 0) {
		return;
	}
}

static int setup_interface_monitoring(struct supplicant_context *ctx, struct net_if *iface)
{
	ARG_UNUSED(iface);
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_INF_MON
	net_mgmt_init_event_callback(&ctx->cb, interface_handler,
				     INTERFACE_EVENT_MASK);
	net_mgmt_add_event_callback(&ctx->cb);
#endif
	net_if_foreach(iface_cb, ctx);

	return 0;
}

static void event_socket_handler(int sock, void *eloop_ctx, void *user_data)
{
	struct supplicant_context *ctx = user_data;
	struct wpa_supplicant_event_msg event_msg;
	struct zephyr_msg *msg;
	zvfs_eventfd_t value;

	ARG_UNUSED(eloop_ctx);

	do {
		zvfs_eventfd_read(sock, &value);

		msg = k_fifo_get(&ctx->fifo, K_NO_WAIT);
		if (msg == NULL) {
			LOG_ERR("fifo(event): %s", "empty");
			return;
		}

		if (msg->data == NULL) {
			LOG_ERR("fifo(event): %s", "no data");
			goto out;
		}

		if (msg->len != sizeof(event_msg)) {
			LOG_ERR("Received incomplete message: got: %d, expected:%d",
				msg->len, sizeof(event_msg));
			goto out;
		}

		memcpy(&event_msg, msg->data, sizeof(event_msg));

		LOG_DBG("Passing message %d to wpa_supplicant", event_msg.event);

		if (event_msg.global) {
			wpa_supplicant_event_global(event_msg.ctx, event_msg.event,
						    event_msg.data);
#ifdef CONFIG_WIFI_NM_HOSTAPD_AP
		} else if (event_msg.hostapd) {
			hostapd_event(event_msg.ctx, event_msg.event, event_msg.data);
#endif
		} else {
			wpa_supplicant_event(event_msg.ctx, event_msg.event, event_msg.data);
		}

		if (event_msg.data) {
			union wpa_event_data *data = event_msg.data;

			/* Free up deep copied data */
			if (event_msg.event == EVENT_AUTH) {
				os_free((char *)data->auth.ies);
			} else if (event_msg.event == EVENT_RX_MGMT) {
				os_free((char *)data->rx_mgmt.frame);
			} else if (event_msg.event == EVENT_TX_STATUS) {
				os_free((char *)data->tx_status.data);
			} else if (event_msg.event == EVENT_ASSOC) {
				os_free((char *)data->assoc_info.addr);
				os_free((char *)data->assoc_info.req_ies);
				os_free((char *)data->assoc_info.resp_ies);
				os_free((char *)data->assoc_info.resp_frame);
			} else if (event_msg.event == EVENT_ASSOC_REJECT) {
				os_free((char *)data->assoc_reject.bssid);
				os_free((char *)data->assoc_reject.resp_ies);
			} else if (event_msg.event == EVENT_DEAUTH) {
				os_free((char *)data->deauth_info.addr);
				os_free((char *)data->deauth_info.ie);
			} else if (event_msg.event == EVENT_DISASSOC) {
				os_free((char *)data->disassoc_info.addr);
				os_free((char *)data->disassoc_info.ie);
			} else if (event_msg.event == EVENT_UNPROT_DEAUTH) {
				os_free((char *)data->unprot_deauth.sa);
				os_free((char *)data->unprot_deauth.da);
			} else if (event_msg.event == EVENT_UNPROT_DISASSOC) {
				os_free((char *)data->unprot_disassoc.sa);
				os_free((char *)data->unprot_disassoc.da);
			}

			os_free(event_msg.data);
		}

out:
		os_free(msg->data);
		os_free(msg);

	} while (!k_fifo_is_empty(&ctx->fifo));
}

static int register_supplicant_event_socket(struct supplicant_context *ctx)
{
	int ret;

	ret = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);
	if (ret < 0) {
		ret = -errno;
		LOG_ERR("Failed to initialize socket (%d)", ret);
		return ret;
	}

	ctx->sock = ret;

	k_fifo_init(&ctx->fifo);

	eloop_register_read_sock(ctx->sock, event_socket_handler, NULL, ctx);

	return 0;
}

static void handler(void)
{
	struct supplicant_context *ctx;
	struct wpa_params params;
	struct k_work_queue_config iface_wq_cfg = {
		.name = "hostap_iface_wq",
	};

#if !defined(CONFIG_WIFI_NM_WPA_SUPPLICANT_CRYPTO_NONE) && !defined(CONFIG_MBEDTLS_ENABLE_HEAP)
	/* Needed for crypto operation as default is no-op and fails */
	mbedtls_platform_set_calloc_free(calloc, free);
#endif /* !CONFIG_WIFI_NM_WPA_SUPPLICANT_CRYPTO_NONE && !CONFIG_MBEDTLS_ENABLE_HEAP */

#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT_CRYPTO_MBEDTLS_PSA
	supp_psa_crypto_init();
#endif

	ctx = get_default_context();

	k_work_queue_init(&ctx->iface_wq);
	k_work_queue_start(&ctx->iface_wq, iface_wq_stack,
			   K_THREAD_STACK_SIZEOF(iface_wq_stack),
			   CONFIG_WIFI_NM_WPA_SUPPLICANT_WQ_PRIO,
			   &iface_wq_cfg);

	k_work_init(&ctx->iface_work, iface_work_handler);

	memset(&params, 0, sizeof(params));
	params.wpa_debug_level = CONFIG_WIFI_NM_WPA_SUPPLICANT_DEBUG_LEVEL;

	ctx->supplicant = wpa_supplicant_init(&params);
	if (ctx->supplicant == NULL) {
		LOG_ERR("Failed to initialize %s", "wpa_supplicant");
		goto err;
	}

	LOG_INF("%s initialized", "wpa_supplicant");

	if (fst_global_init()) {
		LOG_ERR("Failed to initialize %s", "FST");
		goto out;
	}

#if defined(CONFIG_FST) && defined(CONFIG_CTRL_IFACE)
	if (!fst_global_add_ctrl(fst_ctrl_cli)) {
		LOG_WRN("Failed to add CLI FST ctrl");
	}
#endif
	zephyr_global_wpa_ctrl_init();

	register_supplicant_event_socket(ctx);

	submit_iface_work(ctx, NULL, setup_interface_monitoring);

#ifdef CONFIG_WIFI_NM_HOSTAPD_AP
	zephyr_hostapd_init(&ctx->hostapd);
#endif
	wpa_msg_register_ifname_cb(zephyr_hostap_msg_ifname_cb);

	(void)wpa_supplicant_run(ctx->supplicant);

	supplicant_generate_state_event(ctx->if_name, NET_EVENT_SUPPLICANT_CMD_NOT_READY, 0);

	eloop_unregister_read_sock(ctx->sock);

	zephyr_global_wpa_ctrl_deinit();

	fst_global_deinit();

out:
	wpa_supplicant_deinit(ctx->supplicant);

	close(ctx->sock);

err:
	os_free(params.pid_file);
}

static int init(void)
{
	k_tid_t id;

	/* We create a thread that handles all supplicant connections */
	id = k_thread_create(&tid, supplicant_thread_stack,
			     K_THREAD_STACK_SIZEOF(supplicant_thread_stack),
			     (k_thread_entry_t)handler, NULL, NULL, NULL,
			     CONFIG_WIFI_NM_WPA_SUPPLICANT_PRIO, 0, K_NO_WAIT);

	k_thread_name_set(id, "hostap_handler");

	return 0;
}

SYS_INIT(init, APPLICATION, 0);

static enum net_verdict eapol_recv(struct net_if *iface, uint16_t ptype,
				   struct net_pkt *pkt)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(ptype);

	net_pkt_set_family(pkt, AF_UNSPEC);

	return NET_CONTINUE;
}

ETH_NET_L3_REGISTER(EAPOL, NET_ETH_PTYPE_EAPOL, eapol_recv);
