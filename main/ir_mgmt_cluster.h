#pragma once

#include <esp_err.h>
#include <esp_matter.h>

/* ── IrManagement Custom Cluster ─────────────────────────────────────────────
 *  Manufacturer-specific cluster (Cluster ID 0x1337FC01) that exposes the
 *  IR learning / signal management / bridge-slot wiring functionality over
 *  the Matter data model, replacing the HTTP REST API surface.
 * ────────────────────────────────────────────────────────────────────────── */

/* Cluster ID  (VendorID 0x1337 | ClusterID 0xFC01) — suffix must be in 0xFC00-0xFFFE per Matter spec */
static constexpr uint32_t IR_MGMT_CLUSTER_ID = 0x1337FC01;

/* ── Attribute IDs ──────────────────────────────────────────────────────── */
static constexpr uint32_t IR_MGMT_ATTR_LEARN_STATE          = 0x0000;
static constexpr uint32_t IR_MGMT_ATTR_LEARNED_PAYLOAD      = 0x0001;
static constexpr uint32_t IR_MGMT_ATTR_SAVED_SIGNALS_LIST   = 0x0002;
static constexpr uint32_t IR_MGMT_ATTR_SLOT_ASSIGNMENTS     = 0x0003;
static constexpr uint32_t IR_MGMT_ATTR_REGISTERED_DEVICES   = 0x0004;

/* ── Command IDs (client → server, i.e. "accepted") ────────────────────── */
static constexpr uint32_t IR_MGMT_CMD_START_LEARNING        = 0x00;
static constexpr uint32_t IR_MGMT_CMD_CANCEL_LEARNING       = 0x01;
static constexpr uint32_t IR_MGMT_CMD_SAVE_SIGNAL           = 0x02;
static constexpr uint32_t IR_MGMT_CMD_DELETE_SIGNAL         = 0x03;
static constexpr uint32_t IR_MGMT_CMD_ASSIGN_SIGNAL_TO_DEV  = 0x04;
static constexpr uint32_t IR_MGMT_CMD_REGISTER_DEVICE       = 0x05;
static constexpr uint32_t IR_MGMT_CMD_RENAME_DEVICE         = 0x06;
static constexpr uint32_t IR_MGMT_CMD_ASSIGN_DEVICE_TO_SLOT = 0x07;
static constexpr uint32_t IR_MGMT_CMD_OPEN_COMMISSIONING    = 0x08;
static constexpr uint32_t IR_MGMT_CMD_SEND_SIGNAL           = 0x09;
static constexpr uint32_t IR_MGMT_CMD_GET_SIGNAL_PAYLOAD    = 0x0A;

/* ── Attribute IDs (extended) ─────────────────────────────────────────── */
static constexpr uint32_t IR_MGMT_ATTR_SIGNAL_PAYLOAD_DATA  = 0x0005;

/* ── Event IDs ──────────────────────────────────────────────────────────── */
static constexpr uint32_t IR_MGMT_EVT_LEARNING_COMPLETED    = 0x00;

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * Create a dedicated (non-bridged) endpoint and register the IrManagement
 * cluster with all attributes, commands and events.
 *
 * Call this AFTER the Matter node and bridge slot endpoints have been
 * created, but BEFORE esp_matter::start().
 *
 * @param node  The Matter node handle returned by node::create().
 * @return ESP_OK on success.
 */
esp_err_t ir_mgmt_cluster_init(esp_matter::node_t *node);

/** Return the endpoint ID assigned to the IrManagement cluster (0 if not initialised). */
uint16_t ir_mgmt_cluster_get_endpoint_id(void);
