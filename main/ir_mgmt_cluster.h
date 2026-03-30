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
static constexpr uint32_t IR_MGMT_ATTR_BUFFER_SNAPSHOT      = 0x0007;

/* ── Command IDs (client → server, i.e. "accepted") ────────────────────── */
static constexpr uint32_t IR_MGMT_CMD_START_LEARNING        = 0x00;
static constexpr uint32_t IR_MGMT_CMD_CANCEL_LEARNING       = 0x01;
static constexpr uint32_t IR_MGMT_CMD_OPEN_COMMISSIONING    = 0x08;
static constexpr uint32_t IR_MGMT_CMD_SEND_SIGNAL_WITH_RAW  = 0x0B;
static constexpr uint32_t IR_MGMT_CMD_SYNC_BUFFER           = 0x0C;
static constexpr uint32_t IR_MGMT_CMD_FACTORY_RESET         = 0x0D;
static constexpr uint32_t IR_MGMT_CMD_DUMP_NVS              = 0x0E;

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
