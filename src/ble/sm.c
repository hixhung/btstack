/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 *
 */

#define __BTSTACK_FILE__ "sm.c"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "ble/le_device_db.h"
#include "ble/core.h"
#include "ble/sm.h"
#include "bluetooth_company_id.h"
#include "btstack_crypto.h"
#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_linked_list.h"
#include "btstack_memory.h"
#include "gap.h"
#include "hci.h"
#include "hci_dump.h"
#include "l2cap.h"

#if !defined(ENABLE_LE_PERIPHERAL) && !defined(ENABLE_LE_CENTRAL)
#error "LE Security Manager used, but neither ENABLE_LE_PERIPHERAL nor ENABLE_LE_CENTRAL defined. Please add at least one to btstack_config.h."
#endif

// assert SM Public Key can be sent/received
#ifdef ENABLE_LE_SECURE_CONNECTIONS
#if HCI_ACL_PAYLOAD_SIZE < 69
#error "HCI_ACL_PAYLOAD_SIZE must be at least 69 bytes when using LE Secure Conection. Please increase HCI_ACL_PAYLOAD_SIZE or disable ENABLE_LE_SECURE_CONNECTIONS"
#endif
#endif

#if defined(ENABLE_LE_PERIPHERAL) && defined(ENABLE_LE_CENTRAL)
#define IS_RESPONDER(role) (role)
#else
#ifdef ENABLE_LE_CENTRAL
// only central - never responder (avoid 'unused variable' warnings)
#define IS_RESPONDER(role) (0 && role)
#else
// only peripheral - always responder (avoid 'unused variable' warnings)
#define IS_RESPONDER(role) (1 || role)
#endif
#endif

#if defined(ENABLE_LE_SIGNED_WRITE) || defined(ENABLE_LE_SECURE_CONNECTIONS)
#define USE_CMAC_ENGINE
#endif

//
// SM internal types and globals
//

typedef enum {
    DKG_W4_WORKING,
    DKG_CALC_IRK,
    DKG_CALC_DHK,
    DKG_READY
} derived_key_generation_t;

typedef enum {
    RAU_W4_WORKING,
    RAU_IDLE,
    RAU_W4_RANDOM,
    RAU_GET_ENC,
    RAU_W4_ENC,
    RAU_SET_ADDRESS,
} random_address_update_t;

typedef enum {
    CMAC_IDLE,
    CMAC_CALC_SUBKEYS,
    CMAC_W4_SUBKEYS,
    CMAC_CALC_MI,
    CMAC_W4_MI,
    CMAC_CALC_MLAST,
    CMAC_W4_MLAST
} cmac_state_t;

typedef enum {
    JUST_WORKS,
    PK_RESP_INPUT,       // Initiator displays PK, responder inputs PK
    PK_INIT_INPUT,       // Responder displays PK, initiator inputs PK
    PK_BOTH_INPUT,       // Only input on both, both input PK
    NUMERIC_COMPARISON,  // Only numerical compparison (yes/no) on on both sides
    OOB                  // OOB available on one (SC) or both sides (legacy)
} stk_generation_method_t;

typedef enum {
    SM_USER_RESPONSE_IDLE,
    SM_USER_RESPONSE_PENDING,
    SM_USER_RESPONSE_CONFIRM,
    SM_USER_RESPONSE_PASSKEY,
    SM_USER_RESPONSE_DECLINE
} sm_user_response_t;

typedef enum {
    SM_AES128_IDLE,
    SM_AES128_ACTIVE
} sm_aes128_state_t;

typedef enum {
    ADDRESS_RESOLUTION_IDLE,
    ADDRESS_RESOLUTION_GENERAL,
    ADDRESS_RESOLUTION_FOR_CONNECTION,
} address_resolution_mode_t;

typedef enum {
    ADDRESS_RESOLUTION_SUCEEDED,
    ADDRESS_RESOLUTION_FAILED,
} address_resolution_event_t;

typedef enum {
    EC_KEY_GENERATION_ACTIVE,
    EC_KEY_GENERATION_DONE,
} ec_key_generation_state_t;

typedef enum {
    SM_STATE_VAR_DHKEY_NEEDED = 1 << 0,
    SM_STATE_VAR_DHKEY_CALCULATED = 1 << 1,
    SM_STATE_VAR_DHKEY_COMMAND_RECEIVED = 1 << 2,
} sm_state_var_t;

typedef enum {
    SM_SC_OOB_IDLE,
    SM_SC_OOB_W4_RANDOM,
    SM_SC_OOB_W2_CALC_CONFIRM,
    SM_SC_OOB_W4_CONFIRM,
} sm_sc_oob_state_t;

typedef uint8_t sm_key24_t[3];
typedef uint8_t sm_key56_t[7];
typedef uint8_t sm_key256_t[32];

//
// GLOBAL DATA
//

static uint8_t test_use_fixed_local_csrk;

#ifdef ENABLE_TESTING_SUPPORT
static uint8_t test_pairing_failure;
#endif

// configuration
static uint8_t sm_accepted_stk_generation_methods;
static uint8_t sm_max_encryption_key_size;
static uint8_t sm_min_encryption_key_size;
static uint8_t sm_auth_req = 0;
static uint8_t sm_io_capabilities = IO_CAPABILITY_NO_INPUT_NO_OUTPUT;
static uint8_t sm_slave_request_security;
static uint32_t sm_fixed_passkey_in_display_role;
static uint8_t sm_reconstruct_ltk_without_le_device_db_entry;

#ifdef ENABLE_LE_SECURE_CONNECTIONS
static uint8_t sm_sc_oob_random[16];
static void (*sm_sc_oob_callback)(const uint8_t * confirm_value, const uint8_t * random_value);
static sm_sc_oob_state_t sm_sc_oob_state;
#endif

// Security Manager Master Keys, please use sm_set_er(er) and sm_set_ir(ir) with your own 128 bit random values
static sm_key_t sm_persistent_er;
static sm_key_t sm_persistent_ir;

// derived from sm_persistent_ir
static sm_key_t sm_persistent_dhk;
static sm_key_t sm_persistent_irk;
static uint8_t  sm_persistent_irk_ready = 0;    // used for testing
static derived_key_generation_t dkg_state;

// derived from sm_persistent_er
// ..

// random address update
static random_address_update_t rau_state;
static bd_addr_t sm_random_address;

#ifdef USE_CMAC_ENGINE
// CMAC Calculation: General
static btstack_crypto_aes128_cmac_t sm_cmac_request;
static void (*sm_cmac_done_callback)(uint8_t hash[8]);
static uint8_t sm_cmac_active;
static uint8_t sm_cmac_hash[16];
#endif

// CMAC for ATT Signed Writes
#ifdef ENABLE_LE_SIGNED_WRITE
static uint16_t        sm_cmac_signed_write_message_len;
static uint8_t         sm_cmac_signed_write_header[3];
static const uint8_t * sm_cmac_signed_write_message;
static uint8_t         sm_cmac_signed_write_sign_counter[4];
#endif

// CMAC for Secure Connection functions
#ifdef ENABLE_LE_SECURE_CONNECTIONS
static sm_connection_t * sm_cmac_connection;
static uint8_t           sm_cmac_sc_buffer[80];
#endif

// resolvable private address lookup / CSRK calculation
static int       sm_address_resolution_test;
static int       sm_address_resolution_ah_calculation_active;
static uint8_t   sm_address_resolution_addr_type;
static bd_addr_t sm_address_resolution_address;
static void *    sm_address_resolution_context;
static address_resolution_mode_t sm_address_resolution_mode;
static btstack_linked_list_t sm_address_resolution_general_queue;

// aes128 crypto engine.
static sm_aes128_state_t  sm_aes128_state;

// crypto 
static btstack_crypto_random_t   sm_crypto_random_request;
static btstack_crypto_aes128_t   sm_crypto_aes128_request;
#ifdef ENABLE_LE_SECURE_CONNECTIONS
static btstack_crypto_ecc_p256_t sm_crypto_ecc_p256_request;
static btstack_crypto_random_t   sm_crypto_random_oob_request;
#endif

// temp storage for random data
static uint8_t sm_random_data[8];
static uint8_t sm_aes128_key[16];
static uint8_t sm_aes128_plaintext[16];
static uint8_t sm_aes128_ciphertext[16];

// to receive hci events
static btstack_packet_callback_registration_t hci_event_callback_registration;

/* to dispatch sm event */
static btstack_linked_list_t sm_event_handlers;

// LE Secure Connections
#ifdef ENABLE_LE_SECURE_CONNECTIONS
static ec_key_generation_state_t ec_key_generation_state;
static uint8_t ec_q[64];
#endif

//
// Volume 3, Part H, Chapter 24
// "Security shall be initiated by the Security Manager in the device in the master role.
// The device in the slave role shall be the responding device."
// -> master := initiator, slave := responder
//

// data needed for security setup
typedef struct sm_setup_context {

    btstack_timer_source_t sm_timeout;

    // used in all phases
    uint8_t   sm_pairing_failed_reason;

    // user response, (Phase 1 and/or 2)
    uint8_t   sm_user_response;
    uint8_t   sm_keypress_notification; // bitmap: passkey started, digit entered, digit erased, passkey cleared, passkey complete, 3 bit count

    // defines which keys will be send after connection is encrypted - calculated during Phase 1, used Phase 3
    int       sm_key_distribution_send_set;
    int       sm_key_distribution_received_set;

    // Phase 2 (Pairing over SMP)
    stk_generation_method_t sm_stk_generation_method;
    sm_key_t  sm_tk;
    uint8_t   sm_have_oob_data;
    uint8_t   sm_use_secure_connections;

    sm_key_t  sm_c1_t3_value;   // c1 calculation
    sm_pairing_packet_t sm_m_preq; // pairing request - needed only for c1
    sm_pairing_packet_t sm_s_pres; // pairing response - needed only for c1
    sm_key_t  sm_local_random;
    sm_key_t  sm_local_confirm;
    sm_key_t  sm_peer_random;
    sm_key_t  sm_peer_confirm;
    uint8_t   sm_m_addr_type;   // address and type can be removed
    uint8_t   sm_s_addr_type;   //  ''
    bd_addr_t sm_m_address;     //  ''
    bd_addr_t sm_s_address;     //  ''
    sm_key_t  sm_ltk;

    uint8_t   sm_state_vars;
#ifdef ENABLE_LE_SECURE_CONNECTIONS
    uint8_t   sm_peer_q[64];    // also stores random for EC key generation during init
    sm_key_t  sm_peer_nonce;    // might be combined with sm_peer_random
    sm_key_t  sm_local_nonce;   // might be combined with sm_local_random
    sm_key_t  sm_dhkey;
    sm_key_t  sm_peer_dhkey_check;
    sm_key_t  sm_local_dhkey_check;
    sm_key_t  sm_ra;
    sm_key_t  sm_rb;
    sm_key_t  sm_t;             // used for f5 and h6
    sm_key_t  sm_mackey;
    uint8_t   sm_passkey_bit;   // also stores number of generated random bytes for EC key generation
#endif

    // Phase 3

    // key distribution, we generate
    uint16_t  sm_local_y;
    uint16_t  sm_local_div;
    uint16_t  sm_local_ediv;
    uint8_t   sm_local_rand[8];
    sm_key_t  sm_local_ltk;
    sm_key_t  sm_local_csrk;
    sm_key_t  sm_local_irk;
    // sm_local_address/addr_type not needed

    // key distribution, received from peer
    uint16_t  sm_peer_y;
    uint16_t  sm_peer_div;
    uint16_t  sm_peer_ediv;
    uint8_t   sm_peer_rand[8];
    sm_key_t  sm_peer_ltk;
    sm_key_t  sm_peer_irk;
    sm_key_t  sm_peer_csrk;
    uint8_t   sm_peer_addr_type;
    bd_addr_t sm_peer_address;

} sm_setup_context_t;

//
static sm_setup_context_t the_setup;
static sm_setup_context_t * setup = &the_setup;

// active connection - the one for which the_setup is used for
static uint16_t sm_active_connection_handle = HCI_CON_HANDLE_INVALID;

// @returns 1 if oob data is available
// stores oob data in provided 16 byte buffer if not null
static int (*sm_get_oob_data)(uint8_t addres_type, bd_addr_t addr, uint8_t * oob_data) = NULL;
static int (*sm_get_sc_oob_data)(uint8_t addres_type, bd_addr_t addr, uint8_t * oob_sc_peer_confirm, uint8_t * oob_sc_peer_random);

// horizontal: initiator capabilities
// vertial:    responder capabilities
static const stk_generation_method_t stk_generation_method [5] [5] = {
    { JUST_WORKS,      JUST_WORKS,       PK_INIT_INPUT,   JUST_WORKS,    PK_INIT_INPUT },
    { JUST_WORKS,      JUST_WORKS,       PK_INIT_INPUT,   JUST_WORKS,    PK_INIT_INPUT },
    { PK_RESP_INPUT,   PK_RESP_INPUT,    PK_BOTH_INPUT,   JUST_WORKS,    PK_RESP_INPUT },
    { JUST_WORKS,      JUST_WORKS,       JUST_WORKS,      JUST_WORKS,    JUST_WORKS    },
    { PK_RESP_INPUT,   PK_RESP_INPUT,    PK_INIT_INPUT,   JUST_WORKS,    PK_RESP_INPUT },
};

// uses numeric comparison if one side has DisplayYesNo and KeyboardDisplay combinations
#ifdef ENABLE_LE_SECURE_CONNECTIONS
static const stk_generation_method_t stk_generation_method_with_secure_connection[5][5] = {
    { JUST_WORKS,      JUST_WORKS,         PK_INIT_INPUT,   JUST_WORKS,    PK_INIT_INPUT      },
    { JUST_WORKS,      NUMERIC_COMPARISON, PK_INIT_INPUT,   JUST_WORKS,    NUMERIC_COMPARISON },
    { PK_RESP_INPUT,   PK_RESP_INPUT,      PK_BOTH_INPUT,   JUST_WORKS,    PK_RESP_INPUT      },
    { JUST_WORKS,      JUST_WORKS,         JUST_WORKS,      JUST_WORKS,    JUST_WORKS         },
    { PK_RESP_INPUT,   NUMERIC_COMPARISON, PK_INIT_INPUT,   JUST_WORKS,    NUMERIC_COMPARISON },
};
#endif

static void sm_run(void);
#ifdef USE_CMAC_ENGINE
static void sm_cmac_message_start(const sm_key_t key, uint16_t message_len, const uint8_t * message, void (*done_callback)(uint8_t * hash));
#endif
static void sm_done_for_handle(hci_con_handle_t con_handle);
static sm_connection_t * sm_get_connection_for_handle(hci_con_handle_t con_handle);
static inline int sm_calc_actual_encryption_key_size(int other);
static int sm_validate_stk_generation_method(void);
static void sm_handle_encryption_result_address_resolution(void *arg);
static void sm_handle_encryption_result_dkg_dhk(void *arg);
static void sm_handle_encryption_result_dkg_irk(void *arg);
static void sm_handle_encryption_result_enc_a(void *arg);
static void sm_handle_encryption_result_enc_b(void *arg);
static void sm_handle_encryption_result_enc_c(void *arg);
static void sm_handle_encryption_result_enc_csrk(void *arg);
static void sm_handle_encryption_result_enc_d(void * arg);
static void sm_handle_encryption_result_enc_ph3_ltk(void *arg);
static void sm_handle_encryption_result_enc_ph3_y(void *arg);
#ifdef ENABLE_LE_PERIPHERAL
static void sm_handle_encryption_result_enc_ph4_ltk(void *arg);
static void sm_handle_encryption_result_enc_ph4_y(void *arg);
#endif
static void sm_handle_encryption_result_enc_stk(void *arg);
static void sm_handle_encryption_result_rau(void *arg);
static void sm_handle_random_result_ph2_tk(void * arg);
static void sm_handle_random_result_rau(void * arg);
#ifdef ENABLE_LE_SECURE_CONNECTIONS
static void sm_handle_random_result_sc_get_random(void * arg);
static int sm_passkey_entry(stk_generation_method_t method);
#endif
static void sm_notify_client_status_reason(sm_connection_t * sm_conn, uint8_t status, uint8_t reason);

static void log_info_hex16(const char * name, uint16_t value){
    log_info("%-6s 0x%04x", name, value);
}

// static inline uint8_t sm_pairing_packet_get_code(sm_pairing_packet_t packet){
//     return packet[0];
// }
static inline uint8_t sm_pairing_packet_get_io_capability(sm_pairing_packet_t packet){
    return packet[1];
}
static inline uint8_t sm_pairing_packet_get_oob_data_flag(sm_pairing_packet_t packet){
    return packet[2];
}
static inline uint8_t sm_pairing_packet_get_auth_req(sm_pairing_packet_t packet){
    return packet[3];
}
static inline uint8_t sm_pairing_packet_get_max_encryption_key_size(sm_pairing_packet_t packet){
    return packet[4];
}
static inline uint8_t sm_pairing_packet_get_initiator_key_distribution(sm_pairing_packet_t packet){
    return packet[5];
}
static inline uint8_t sm_pairing_packet_get_responder_key_distribution(sm_pairing_packet_t packet){
    return packet[6];
}

static inline void sm_pairing_packet_set_code(sm_pairing_packet_t packet, uint8_t code){
    packet[0] = code;
}
static inline void sm_pairing_packet_set_io_capability(sm_pairing_packet_t packet, uint8_t io_capability){
    packet[1] = io_capability;
}
static inline void sm_pairing_packet_set_oob_data_flag(sm_pairing_packet_t packet, uint8_t oob_data_flag){
    packet[2] = oob_data_flag;
}
static inline void sm_pairing_packet_set_auth_req(sm_pairing_packet_t packet, uint8_t auth_req){
    packet[3] = auth_req;
}
static inline void sm_pairing_packet_set_max_encryption_key_size(sm_pairing_packet_t packet, uint8_t max_encryption_key_size){
    packet[4] = max_encryption_key_size;
}
static inline void sm_pairing_packet_set_initiator_key_distribution(sm_pairing_packet_t packet, uint8_t initiator_key_distribution){
    packet[5] = initiator_key_distribution;
}
static inline void sm_pairing_packet_set_responder_key_distribution(sm_pairing_packet_t packet, uint8_t responder_key_distribution){
    packet[6] = responder_key_distribution;
}

// @returns 1 if all bytes are 0
static int sm_is_null(uint8_t * data, int size){
    int i;
    for (i=0; i < size ; i++){
        if (data[i]) return 0;
    }
    return 1;
}

static int sm_is_null_random(uint8_t random[8]){
    return sm_is_null(random, 8);
}

static int sm_is_null_key(uint8_t * key){
    return sm_is_null(key, 16);
}

// Key utils
static void sm_reset_tk(void){
    int i;
    for (i=0;i<16;i++){
        setup->sm_tk[i] = 0;
    }
}

// "For example, if a 128-bit encryption key is 0x123456789ABCDEF0123456789ABCDEF0
// and it is reduced to 7 octets (56 bits), then the resulting key is 0x0000000000000000003456789ABCDEF0.""
static void sm_truncate_key(sm_key_t key, int max_encryption_size){
    int i;
    for (i = max_encryption_size ; i < 16 ; i++){
        key[15-i] = 0;
    }
}

// SMP Timeout implementation

// Upon transmission of the Pairing Request command or reception of the Pairing Request command,
// the Security Manager Timer shall be reset and started.
//
// The Security Manager Timer shall be reset when an L2CAP SMP command is queued for transmission.
//
// If the Security Manager Timer reaches 30 seconds, the procedure shall be considered to have failed,
// and the local higher layer shall be notified. No further SMP commands shall be sent over the L2CAP
// Security Manager Channel. A new SM procedure shall only be performed when a new physical link has been
// established.

static void sm_timeout_handler(btstack_timer_source_t * timer){
    log_info("SM timeout");
    sm_connection_t * sm_conn = (sm_connection_t*) btstack_run_loop_get_timer_context(timer);
    sm_conn->sm_engine_state = SM_GENERAL_TIMEOUT;
    sm_notify_client_status_reason(sm_conn, ERROR_CODE_CONNECTION_TIMEOUT, 0);
    sm_done_for_handle(sm_conn->sm_handle);

    // trigger handling of next ready connection
    sm_run();
}
static void sm_timeout_start(sm_connection_t * sm_conn){
    btstack_run_loop_remove_timer(&setup->sm_timeout);
    btstack_run_loop_set_timer_context(&setup->sm_timeout, sm_conn);
    btstack_run_loop_set_timer_handler(&setup->sm_timeout, sm_timeout_handler);
    btstack_run_loop_set_timer(&setup->sm_timeout, 30000); // 30 seconds sm timeout
    btstack_run_loop_add_timer(&setup->sm_timeout);
}
static void sm_timeout_stop(void){
    btstack_run_loop_remove_timer(&setup->sm_timeout);
}
static void sm_timeout_reset(sm_connection_t * sm_conn){
    sm_timeout_stop();
    sm_timeout_start(sm_conn);
}

// end of sm timeout

// GAP Random Address updates
static gap_random_address_type_t gap_random_adress_type;
static btstack_timer_source_t gap_random_address_update_timer;
static uint32_t gap_random_adress_update_period;

static void gap_random_address_trigger(void){
    log_info("gap_random_address_trigger");
    if (rau_state != RAU_IDLE) return;
    rau_state = RAU_W4_RANDOM;
    btstack_crypto_random_generate(&sm_crypto_random_request, sm_random_address, 8, &sm_handle_random_result_rau, NULL);
}

static void gap_random_address_update_handler(btstack_timer_source_t * timer){
    UNUSED(timer);

    log_info("GAP Random Address Update due");
    btstack_run_loop_set_timer(&gap_random_address_update_timer, gap_random_adress_update_period);
    btstack_run_loop_add_timer(&gap_random_address_update_timer);
    gap_random_address_trigger();
}

static void gap_random_address_update_start(void){
    btstack_run_loop_set_timer_handler(&gap_random_address_update_timer, gap_random_address_update_handler);
    btstack_run_loop_set_timer(&gap_random_address_update_timer, gap_random_adress_update_period);
    btstack_run_loop_add_timer(&gap_random_address_update_timer);
}

static void gap_random_address_update_stop(void){
    btstack_run_loop_remove_timer(&gap_random_address_update_timer);
}

// ah(k,r) helper
// r = padding || r
// r - 24 bit value
static void sm_ah_r_prime(uint8_t r[3], uint8_t * r_prime){
    // r'= padding || r
    memset(r_prime, 0, 16);
    memcpy(&r_prime[13], r, 3);
}

// d1 helper
// d' = padding || r || d
// d,r - 16 bit values
static void sm_d1_d_prime(uint16_t d, uint16_t r, uint8_t * d1_prime){
    // d'= padding || r || d
    memset(d1_prime, 0, 16);
    big_endian_store_16(d1_prime, 12, r);
    big_endian_store_16(d1_prime, 14, d);
}

// dm helper
// r’ = padding || r
// r - 64 bit value
static void sm_dm_r_prime(uint8_t r[8], uint8_t * r_prime){
    memset(r_prime, 0, 16);
    memcpy(&r_prime[8], r, 8);
}

// calculate arguments for first AES128 operation in C1 function
static void sm_c1_t1(sm_key_t r, uint8_t preq[7], uint8_t pres[7], uint8_t iat, uint8_t rat, uint8_t * t1){

    // p1 = pres || preq || rat’ || iat’
    // "The octet of iat’ becomes the least significant octet of p1 and the most signifi-
    // cant octet of pres becomes the most significant octet of p1.
    // For example, if the 8-bit iat’ is 0x01, the 8-bit rat’ is 0x00, the 56-bit preq
    // is 0x07071000000101 and the 56 bit pres is 0x05000800000302 then
    // p1 is 0x05000800000302070710000001010001."

    sm_key_t p1;
    reverse_56(pres, &p1[0]);
    reverse_56(preq, &p1[7]);
    p1[14] = rat;
    p1[15] = iat;
    log_info_key("p1", p1);
    log_info_key("r", r);

    // t1 = r xor p1
    int i;
    for (i=0;i<16;i++){
        t1[i] = r[i] ^ p1[i];
    }
    log_info_key("t1", t1);
}

// calculate arguments for second AES128 operation in C1 function
static void sm_c1_t3(sm_key_t t2, bd_addr_t ia, bd_addr_t ra, uint8_t * t3){
     // p2 = padding || ia || ra
    // "The least significant octet of ra becomes the least significant octet of p2 and
    // the most significant octet of padding becomes the most significant octet of p2.
    // For example, if 48-bit ia is 0xA1A2A3A4A5A6 and the 48-bit ra is
    // 0xB1B2B3B4B5B6 then p2 is 0x00000000A1A2A3A4A5A6B1B2B3B4B5B6.

    sm_key_t p2;
    memset(p2, 0, 16);
    memcpy(&p2[4],  ia, 6);
    memcpy(&p2[10], ra, 6);
    log_info_key("p2", p2);

    // c1 = e(k, t2_xor_p2)
    int i;
    for (i=0;i<16;i++){
        t3[i] = t2[i] ^ p2[i];
    }
    log_info_key("t3", t3);
}

static void sm_s1_r_prime(sm_key_t r1, sm_key_t r2, uint8_t * r_prime){
    log_info_key("r1", r1);
    log_info_key("r2", r2);
    memcpy(&r_prime[8], &r2[8], 8);
    memcpy(&r_prime[0], &r1[8], 8);
}

static void sm_dispatch_event(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel);

    // log event
    hci_dump_packet(packet_type, 1, packet, size);
    // dispatch to all event handlers
    btstack_linked_list_iterator_t it;
    btstack_linked_list_iterator_init(&it, &sm_event_handlers);
    while (btstack_linked_list_iterator_has_next(&it)){
        btstack_packet_callback_registration_t * entry = (btstack_packet_callback_registration_t*) btstack_linked_list_iterator_next(&it);
        entry->callback(packet_type, 0, packet, size);
    }
}

static void sm_setup_event_base(uint8_t * event, int event_size, uint8_t type, hci_con_handle_t con_handle, uint8_t addr_type, bd_addr_t address){
    event[0] = type;
    event[1] = event_size - 2;
    little_endian_store_16(event, 2, con_handle);
    event[4] = addr_type;
    reverse_bd_addr(address, &event[5]);
}

static void sm_notify_client_base(uint8_t type, hci_con_handle_t con_handle, uint8_t addr_type, bd_addr_t address){
    uint8_t event[11];
    sm_setup_event_base(event, sizeof(event), type, con_handle, addr_type, address);
    sm_dispatch_event(HCI_EVENT_PACKET, 0, event, sizeof(event));
}

static void sm_notify_client_passkey(uint8_t type, hci_con_handle_t con_handle, uint8_t addr_type, bd_addr_t address, uint32_t passkey){
    uint8_t event[15];
    sm_setup_event_base(event, sizeof(event), type, con_handle, addr_type, address);
    little_endian_store_32(event, 11, passkey);
    sm_dispatch_event(HCI_EVENT_PACKET, 0, event, sizeof(event));
}

static void sm_notify_client_index(uint8_t type, hci_con_handle_t con_handle, uint8_t addr_type, bd_addr_t address, uint16_t index){
    // fetch addr and addr type from db
    bd_addr_t identity_address;
    int identity_address_type;
    le_device_db_info(index, &identity_address_type, identity_address, NULL);

    uint8_t event[19];
    sm_setup_event_base(event, sizeof(event), type, con_handle, addr_type, address);
    event[11] = identity_address_type;
    reverse_bd_addr(identity_address, &event[12]);
    event[18] = index;
    sm_dispatch_event(HCI_EVENT_PACKET, 0, event, sizeof(event));
}

static void sm_notify_client_status(uint8_t type, hci_con_handle_t con_handle, uint8_t addr_type, bd_addr_t address, uint8_t status){
    uint8_t event[12];
    sm_setup_event_base(event, sizeof(event), type, con_handle, addr_type, address);
    event[11] = status;
    sm_dispatch_event(HCI_EVENT_PACKET, 0, (uint8_t*) &event, sizeof(event));
}

static void sm_notify_client_status_reason(sm_connection_t * sm_conn, uint8_t status, uint8_t reason){
    uint8_t event[13];
    sm_setup_event_base(event, sizeof(event), SM_EVENT_PAIRING_COMPLETE, sm_conn->sm_handle, setup->sm_peer_addr_type, setup->sm_peer_address);
    event[11] = status;
    event[12] = reason;
    sm_dispatch_event(HCI_EVENT_PACKET, 0, (uint8_t*) &event, sizeof(event));
}

// decide on stk generation based on
// - pairing request
// - io capabilities
// - OOB data availability
static void sm_setup_tk(void){

    // default: just works
    setup->sm_stk_generation_method = JUST_WORKS;

#ifdef ENABLE_LE_SECURE_CONNECTIONS
    setup->sm_use_secure_connections = ( sm_pairing_packet_get_auth_req(setup->sm_m_preq)
                                       & sm_pairing_packet_get_auth_req(setup->sm_s_pres)
                                       & SM_AUTHREQ_SECURE_CONNECTION ) != 0;
#else
    setup->sm_use_secure_connections = 0;
#endif
    log_info("Secure pairing: %u", setup->sm_use_secure_connections);


    // decide if OOB will be used based on SC vs. Legacy and oob flags
    int use_oob = 0;
    if (setup->sm_use_secure_connections){
        // In LE Secure Connections pairing, the out of band method is used if at least
        // one device has the peer device's out of band authentication data available.
        use_oob = sm_pairing_packet_get_oob_data_flag(setup->sm_m_preq) | sm_pairing_packet_get_oob_data_flag(setup->sm_s_pres);
    } else {
        // In LE legacy pairing, the out of band method is used if both the devices have
        // the other device's out of band authentication data available. 
        use_oob = sm_pairing_packet_get_oob_data_flag(setup->sm_m_preq) & sm_pairing_packet_get_oob_data_flag(setup->sm_s_pres);
    }
    if (use_oob){
        log_info("SM: have OOB data");
        log_info_key("OOB", setup->sm_tk);
        setup->sm_stk_generation_method = OOB;
        return;
    }

    // If both devices have not set the MITM option in the Authentication Requirements
    // Flags, then the IO capabilities shall be ignored and the Just Works association
    // model shall be used.
    if (((sm_pairing_packet_get_auth_req(setup->sm_m_preq) & SM_AUTHREQ_MITM_PROTECTION) == 0)
    &&  ((sm_pairing_packet_get_auth_req(setup->sm_s_pres) & SM_AUTHREQ_MITM_PROTECTION) == 0)){
        log_info("SM: MITM not required by both -> JUST WORKS");
        return;
    }

    // Reset TK as it has been setup in sm_init_setup
    sm_reset_tk();

    // Also use just works if unknown io capabilites
    if ((sm_pairing_packet_get_io_capability(setup->sm_m_preq) > IO_CAPABILITY_KEYBOARD_DISPLAY) || (sm_pairing_packet_get_io_capability(setup->sm_s_pres) > IO_CAPABILITY_KEYBOARD_DISPLAY)){
        return;
    }

    // Otherwise the IO capabilities of the devices shall be used to determine the
    // pairing method as defined in Table 2.4.
    // see http://stackoverflow.com/a/1052837/393697 for how to specify pointer to 2-dimensional array
    const stk_generation_method_t (*generation_method)[5] = stk_generation_method;

#ifdef ENABLE_LE_SECURE_CONNECTIONS
    // table not define by default
    if (setup->sm_use_secure_connections){
        generation_method = stk_generation_method_with_secure_connection;
    }
#endif
    setup->sm_stk_generation_method = generation_method[sm_pairing_packet_get_io_capability(setup->sm_s_pres)][sm_pairing_packet_get_io_capability(setup->sm_m_preq)];

    log_info("sm_setup_tk: master io cap: %u, slave io cap: %u -> method %u",
        sm_pairing_packet_get_io_capability(setup->sm_m_preq), sm_pairing_packet_get_io_capability(setup->sm_s_pres), setup->sm_stk_generation_method);
}

static int sm_key_distribution_flags_for_set(uint8_t key_set){
    int flags = 0;
    if (key_set & SM_KEYDIST_ENC_KEY){
        flags |= SM_KEYDIST_FLAG_ENCRYPTION_INFORMATION;
        flags |= SM_KEYDIST_FLAG_MASTER_IDENTIFICATION;
    }
    if (key_set & SM_KEYDIST_ID_KEY){
        flags |= SM_KEYDIST_FLAG_IDENTITY_INFORMATION;
        flags |= SM_KEYDIST_FLAG_IDENTITY_ADDRESS_INFORMATION;
    }
    if (key_set & SM_KEYDIST_SIGN){
        flags |= SM_KEYDIST_FLAG_SIGNING_IDENTIFICATION;
    }
    return flags;
}

static void sm_setup_key_distribution(uint8_t key_set){
    setup->sm_key_distribution_received_set = 0;
    setup->sm_key_distribution_send_set = sm_key_distribution_flags_for_set(key_set);
}

// CSRK Key Lookup


static int sm_address_resolution_idle(void){
    return sm_address_resolution_mode == ADDRESS_RESOLUTION_IDLE;
}

static void sm_address_resolution_start_lookup(uint8_t addr_type, hci_con_handle_t con_handle, bd_addr_t addr, address_resolution_mode_t mode, void * context){
    memcpy(sm_address_resolution_address, addr, 6);
    sm_address_resolution_addr_type = addr_type;
    sm_address_resolution_test = 0;
    sm_address_resolution_mode = mode;
    sm_address_resolution_context = context;
    sm_notify_client_base(SM_EVENT_IDENTITY_RESOLVING_STARTED, con_handle, addr_type, addr);
}

int sm_address_resolution_lookup(uint8_t address_type, bd_addr_t address){
    // check if already in list
    btstack_linked_list_iterator_t it;
    sm_lookup_entry_t * entry;
    btstack_linked_list_iterator_init(&it, &sm_address_resolution_general_queue);
    while(btstack_linked_list_iterator_has_next(&it)){
        entry = (sm_lookup_entry_t *) btstack_linked_list_iterator_next(&it);
        if (entry->address_type != address_type) continue;
        if (memcmp(entry->address, address, 6))  continue;
        // already in list
        return BTSTACK_BUSY;
    }
    entry = btstack_memory_sm_lookup_entry_get();
    if (!entry) return BTSTACK_MEMORY_ALLOC_FAILED;
    entry->address_type = (bd_addr_type_t) address_type;
    memcpy(entry->address, address, 6);
    btstack_linked_list_add(&sm_address_resolution_general_queue, (btstack_linked_item_t *) entry);
    sm_run();
    return 0;
}

// CMAC calculation using AES Engineq
#ifdef USE_CMAC_ENGINE

static void sm_cmac_done_trampoline(void * arg){
    UNUSED(arg);
    sm_cmac_active = 0;
    (*sm_cmac_done_callback)(sm_cmac_hash);
    sm_run();
}

int sm_cmac_ready(void){
    return sm_cmac_active == 0;
}

// generic cmac calculation
static void sm_cmac_message_start(const sm_key_t key, uint16_t message_len, const uint8_t * message, void (*done_callback)(uint8_t * hash)){
    sm_cmac_active = 1;
    sm_cmac_done_callback = done_callback;
    btstack_crypto_aes128_cmac_message(&sm_cmac_request, key, message_len, message, sm_cmac_hash, sm_cmac_done_trampoline, NULL);
}
#endif

// cmac for ATT Message signing
#ifdef ENABLE_LE_SIGNED_WRITE

static void sm_cmac_generator_start(const sm_key_t key, uint16_t message_len, uint8_t (*get_byte_callback)(uint16_t offset), void (*done_callback)(uint8_t * hash)){
    sm_cmac_active = 1;
    sm_cmac_done_callback = done_callback;
    btstack_crypto_aes128_cmac_generator(&sm_cmac_request, key, message_len, get_byte_callback, sm_cmac_hash, sm_cmac_done_trampoline, NULL);
}

static uint8_t sm_cmac_signed_write_message_get_byte(uint16_t offset){
    if (offset >= sm_cmac_signed_write_message_len) {
        log_error("sm_cmac_signed_write_message_get_byte. out of bounds, access %u, len %u", offset, sm_cmac_signed_write_message_len);
        return 0;
    }

    offset = sm_cmac_signed_write_message_len - 1 - offset;

    // sm_cmac_signed_write_header[3] | message[] | sm_cmac_signed_write_sign_counter[4]
    if (offset < 3){
        return sm_cmac_signed_write_header[offset];
    }
    int actual_message_len_incl_header = sm_cmac_signed_write_message_len - 4;
    if (offset <  actual_message_len_incl_header){
        return sm_cmac_signed_write_message[offset - 3];
    }
    return sm_cmac_signed_write_sign_counter[offset - actual_message_len_incl_header];
}

void sm_cmac_signed_write_start(const sm_key_t k, uint8_t opcode, hci_con_handle_t con_handle, uint16_t message_len, const uint8_t * message, uint32_t sign_counter, void (*done_handler)(uint8_t * hash)){
    // ATT Message Signing
    sm_cmac_signed_write_header[0] = opcode;
    little_endian_store_16(sm_cmac_signed_write_header, 1, con_handle);
    little_endian_store_32(sm_cmac_signed_write_sign_counter, 0, sign_counter);
    uint16_t total_message_len = 3 + message_len + 4;  // incl. virtually prepended att opcode, handle and appended sign_counter in LE
    sm_cmac_signed_write_message     = message;
    sm_cmac_signed_write_message_len = total_message_len;
    sm_cmac_generator_start(k, total_message_len, &sm_cmac_signed_write_message_get_byte, done_handler);
}
#endif

static void sm_trigger_user_response(sm_connection_t * sm_conn){
    // notify client for: JUST WORKS confirm, Numeric comparison confirm, PASSKEY display or input
    setup->sm_user_response = SM_USER_RESPONSE_IDLE;
    switch (setup->sm_stk_generation_method){
        case PK_RESP_INPUT:
            if (IS_RESPONDER(sm_conn->sm_role)){
                setup->sm_user_response = SM_USER_RESPONSE_PENDING;
                sm_notify_client_base(SM_EVENT_PASSKEY_INPUT_NUMBER, sm_conn->sm_handle, sm_conn->sm_peer_addr_type, sm_conn->sm_peer_address);
            } else {
                sm_notify_client_passkey(SM_EVENT_PASSKEY_DISPLAY_NUMBER, sm_conn->sm_handle, sm_conn->sm_peer_addr_type, sm_conn->sm_peer_address, big_endian_read_32(setup->sm_tk, 12));
            }
            break;
        case PK_INIT_INPUT:
            if (IS_RESPONDER(sm_conn->sm_role)){
                sm_notify_client_passkey(SM_EVENT_PASSKEY_DISPLAY_NUMBER, sm_conn->sm_handle, sm_conn->sm_peer_addr_type, sm_conn->sm_peer_address, big_endian_read_32(setup->sm_tk, 12));
            } else {
                setup->sm_user_response = SM_USER_RESPONSE_PENDING;
                sm_notify_client_base(SM_EVENT_PASSKEY_INPUT_NUMBER, sm_conn->sm_handle, sm_conn->sm_peer_addr_type, sm_conn->sm_peer_address);
            }
            break;
        case PK_BOTH_INPUT:
            setup->sm_user_response = SM_USER_RESPONSE_PENDING;
            sm_notify_client_base(SM_EVENT_PASSKEY_INPUT_NUMBER, sm_conn->sm_handle, sm_conn->sm_peer_addr_type, sm_conn->sm_peer_address);
            break;
        case NUMERIC_COMPARISON:
            setup->sm_user_response = SM_USER_RESPONSE_PENDING;
            sm_notify_client_passkey(SM_EVENT_NUMERIC_COMPARISON_REQUEST, sm_conn->sm_handle, sm_conn->sm_peer_addr_type, sm_conn->sm_peer_address, big_endian_read_32(setup->sm_tk, 12));
            break;
        case JUST_WORKS:
            setup->sm_user_response = SM_USER_RESPONSE_PENDING;
            sm_notify_client_base(SM_EVENT_JUST_WORKS_REQUEST, sm_conn->sm_handle, sm_conn->sm_peer_addr_type, sm_conn->sm_peer_address);
            break;
        case OOB:
            // client already provided OOB data, let's skip notification.
            break;
    }
}

static int sm_key_distribution_all_received(sm_connection_t * sm_conn){
    int recv_flags;
    if (IS_RESPONDER(sm_conn->sm_role)){
        // slave / responder
        recv_flags = sm_key_distribution_flags_for_set(sm_pairing_packet_get_initiator_key_distribution(setup->sm_s_pres));
    } else {
        // master / initiator
        recv_flags = sm_key_distribution_flags_for_set(sm_pairing_packet_get_responder_key_distribution(setup->sm_s_pres));
    }

#ifdef ENABLE_LE_SECURE_CONNECTIONS
    // LTK (= encyrption information & master identification) only used exchanged for LE Legacy Connection
    if (setup->sm_use_secure_connections){
        recv_flags &= ~(SM_KEYDIST_FLAG_ENCRYPTION_INFORMATION | SM_KEYDIST_FLAG_MASTER_IDENTIFICATION);
    }
#endif

    log_debug("sm_key_distribution_all_received: received 0x%02x, expecting 0x%02x", setup->sm_key_distribution_received_set, recv_flags);
    return recv_flags == setup->sm_key_distribution_received_set;
}

static void sm_done_for_handle(hci_con_handle_t con_handle){
    if (sm_active_connection_handle == con_handle){
        sm_timeout_stop();
        sm_active_connection_handle = HCI_CON_HANDLE_INVALID;
        log_info("sm: connection 0x%x released setup context", con_handle);
    }
}

static int sm_key_distribution_flags_for_auth_req(void){

    int flags = SM_KEYDIST_ID_KEY;
    if (sm_auth_req & SM_AUTHREQ_BONDING){
        // encryption and signing information only if bonding requested
        flags |= SM_KEYDIST_ENC_KEY;
#ifdef ENABLE_LE_SIGNED_WRITE
        flags |= SM_KEYDIST_SIGN;
#endif
    }
    return flags;
}

static void sm_reset_setup(void){
    // fill in sm setup
    setup->sm_state_vars = 0;
    setup->sm_keypress_notification = 0;
    sm_reset_tk();
}

static void sm_init_setup(sm_connection_t * sm_conn){

    // fill in sm setup
    setup->sm_peer_addr_type = sm_conn->sm_peer_addr_type;
    memcpy(setup->sm_peer_address, sm_conn->sm_peer_address, 6);

    // query client for Legacy Pairing OOB data
    setup->sm_have_oob_data = 0;
    if (sm_get_oob_data) {
        setup->sm_have_oob_data = (*sm_get_oob_data)(sm_conn->sm_peer_addr_type, sm_conn->sm_peer_address, setup->sm_tk);
    }

    // if available and SC supported, also ask for SC OOB Data
#ifdef ENABLE_LE_SECURE_CONNECTIONS
    memset(setup->sm_ra, 0, 16);
    memset(setup->sm_rb, 0, 16);
    if (setup->sm_have_oob_data && (sm_auth_req & SM_AUTHREQ_SECURE_CONNECTION)){
        if (sm_get_sc_oob_data){
            if (IS_RESPONDER(sm_conn->sm_role)){
                setup->sm_have_oob_data = (*sm_get_sc_oob_data)(
                    sm_conn->sm_peer_addr_type,
                    sm_conn->sm_peer_address,
                    setup->sm_peer_confirm,
                    setup->sm_ra);
            } else {
                setup->sm_have_oob_data = (*sm_get_sc_oob_data)(
                    sm_conn->sm_peer_addr_type,
                    sm_conn->sm_peer_address,
                    setup->sm_peer_confirm,
                    setup->sm_rb);
            }
        } else {
            setup->sm_have_oob_data = 0;
        }
    }
#endif

    sm_pairing_packet_t * local_packet;
    if (IS_RESPONDER(sm_conn->sm_role)){
        // slave
        local_packet = &setup->sm_s_pres;
        gap_le_get_own_address(&setup->sm_s_addr_type, setup->sm_s_address);
        setup->sm_m_addr_type = sm_conn->sm_peer_addr_type;
        memcpy(setup->sm_m_address, sm_conn->sm_peer_address, 6);
    } else {
        // master
        local_packet = &setup->sm_m_preq;
        gap_le_get_own_address(&setup->sm_m_addr_type, setup->sm_m_address);
        setup->sm_s_addr_type = sm_conn->sm_peer_addr_type;
        memcpy(setup->sm_s_address, sm_conn->sm_peer_address, 6);

        int key_distribution_flags = sm_key_distribution_flags_for_auth_req();
        sm_pairing_packet_set_initiator_key_distribution(setup->sm_m_preq, key_distribution_flags);
        sm_pairing_packet_set_responder_key_distribution(setup->sm_m_preq, key_distribution_flags);
    }

    uint8_t auth_req = sm_auth_req;
    sm_pairing_packet_set_io_capability(*local_packet, sm_io_capabilities);
    sm_pairing_packet_set_oob_data_flag(*local_packet, setup->sm_have_oob_data);
    sm_pairing_packet_set_auth_req(*local_packet, auth_req);
    sm_pairing_packet_set_max_encryption_key_size(*local_packet, sm_max_encryption_key_size);
}

static int sm_stk_generation_init(sm_connection_t * sm_conn){

    sm_pairing_packet_t * remote_packet;
    int                   remote_key_request;
    if (IS_RESPONDER(sm_conn->sm_role)){
        // slave / responder
        remote_packet      = &setup->sm_m_preq;
        remote_key_request = sm_pairing_packet_get_responder_key_distribution(setup->sm_m_preq);
    } else {
        // master / initiator
        remote_packet      = &setup->sm_s_pres;
        remote_key_request = sm_pairing_packet_get_initiator_key_distribution(setup->sm_s_pres);
    }

    // check key size
    sm_conn->sm_actual_encryption_key_size = sm_calc_actual_encryption_key_size(sm_pairing_packet_get_max_encryption_key_size(*remote_packet));
    if (sm_conn->sm_actual_encryption_key_size == 0) return SM_REASON_ENCRYPTION_KEY_SIZE;

    // decide on STK generation method / SC
    sm_setup_tk();
    log_info("SMP: generation method %u", setup->sm_stk_generation_method);

    // check if STK generation method is acceptable by client
    if (!sm_validate_stk_generation_method()) return SM_REASON_AUTHENTHICATION_REQUIREMENTS;

#ifdef ENABLE_LE_SECURE_CONNECTIONS
    // LTK (= encyrption information & master identification) only used exchanged for LE Legacy Connection
    if (setup->sm_use_secure_connections){
        remote_key_request &= ~SM_KEYDIST_ENC_KEY;
    }
#endif

    // identical to responder
    sm_setup_key_distribution(remote_key_request);

    // JUST WORKS doens't provide authentication
    sm_conn->sm_connection_authenticated = setup->sm_stk_generation_method == JUST_WORKS ? 0 : 1;

    return 0;
}

static void sm_address_resolution_handle_event(address_resolution_event_t event){

    // cache and reset context
    int matched_device_id = sm_address_resolution_test;
    address_resolution_mode_t mode = sm_address_resolution_mode;
    void * context = sm_address_resolution_context;

    // reset context
    sm_address_resolution_mode = ADDRESS_RESOLUTION_IDLE;
    sm_address_resolution_context = NULL;
    sm_address_resolution_test = -1;
    hci_con_handle_t con_handle = 0;

    sm_connection_t * sm_connection;
#ifdef ENABLE_LE_CENTRAL
    sm_key_t ltk;
    int have_ltk;
#endif
    switch (mode){
        case ADDRESS_RESOLUTION_GENERAL:
            break;
        case ADDRESS_RESOLUTION_FOR_CONNECTION:
            sm_connection = (sm_connection_t *) context;
            con_handle = sm_connection->sm_handle;
            switch (event){
                case ADDRESS_RESOLUTION_SUCEEDED:
                    sm_connection->sm_irk_lookup_state = IRK_LOOKUP_SUCCEEDED;
                    sm_connection->sm_le_db_index = matched_device_id;
                    log_info("ADDRESS_RESOLUTION_SUCEEDED, index %d", sm_connection->sm_le_db_index);
                    if (sm_connection->sm_role) {
                        // LTK request received before, IRK required -> start LTK calculation
                        if (sm_connection->sm_engine_state == SM_RESPONDER_PH0_RECEIVED_LTK_W4_IRK){
                            sm_connection->sm_engine_state = SM_RESPONDER_PH0_RECEIVED_LTK_REQUEST;
                        }
                        break;
                    }
#ifdef ENABLE_LE_CENTRAL
                    le_device_db_encryption_get(sm_connection->sm_le_db_index, NULL, NULL, ltk, NULL, NULL, NULL);
                    have_ltk = !sm_is_null_key(ltk);
                    log_info("central: bonding requested %u, remote security request %u, have ltk %u", sm_connection->sm_pairing_requested, sm_connection->sm_security_request_received, have_ltk);
                    if (!sm_connection->sm_pairing_requested && !sm_connection->sm_security_request_received) break;
                    sm_connection->sm_security_request_received = 0;
                    sm_connection->sm_pairing_requested = 0;
                    if (!sm_is_null_key(ltk)){
                        sm_connection->sm_engine_state = SM_INITIATOR_PH0_HAS_LTK;
                    } else {
                        sm_connection->sm_engine_state = SM_INITIATOR_PH1_W2_SEND_PAIRING_REQUEST;
                    }
#endif
                    break;
                case ADDRESS_RESOLUTION_FAILED:
                    sm_connection->sm_irk_lookup_state = IRK_LOOKUP_FAILED;
                    if (sm_connection->sm_role) {
                        // LTK request received before, IRK required -> negative LTK reply
                        if (sm_connection->sm_engine_state == SM_RESPONDER_PH0_RECEIVED_LTK_W4_IRK){
                            sm_connection->sm_engine_state = SM_RESPONDER_PH0_SEND_LTK_REQUESTED_NEGATIVE_REPLY;
                        }
                        break;
                    }
#ifdef ENABLE_LE_CENTRAL
                    if (!sm_connection->sm_pairing_requested && !sm_connection->sm_security_request_received) break;
                    sm_connection->sm_security_request_received = 0;
                    sm_connection->sm_pairing_requested = 0;
                    sm_connection->sm_engine_state = SM_INITIATOR_PH1_W2_SEND_PAIRING_REQUEST;
#endif
                    break;
            }
            break;
        default:
            break;
    }

    switch (event){
        case ADDRESS_RESOLUTION_SUCEEDED:
            sm_notify_client_index(SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED, con_handle, sm_address_resolution_addr_type, sm_address_resolution_address, matched_device_id);
            break;
        case ADDRESS_RESOLUTION_FAILED:
            sm_notify_client_base(SM_EVENT_IDENTITY_RESOLVING_FAILED, con_handle, sm_address_resolution_addr_type, sm_address_resolution_address);
            break;
    }
}

static void sm_key_distribution_handle_all_received(sm_connection_t * sm_conn){

    int le_db_index = -1;

    // only store pairing information if both sides are bondable, i.e., the bonadble flag is set
    int bonding_enabed = ( sm_pairing_packet_get_auth_req(setup->sm_m_preq)
                         & sm_pairing_packet_get_auth_req(setup->sm_s_pres)
                         & SM_AUTHREQ_BONDING ) != 0;

    if (bonding_enabed){

        // lookup device based on IRK
        if (setup->sm_key_distribution_received_set & SM_KEYDIST_FLAG_IDENTITY_INFORMATION){
            int i;
            for (i=0; i < le_device_db_max_count(); i++){
                sm_key_t irk;
                bd_addr_t address;
                int address_type;
                le_device_db_info(i, &address_type, address, irk);
                if (memcmp(irk, setup->sm_peer_irk, 16) == 0){
                    log_info("sm: device found for IRK, updating");
                    le_db_index = i;
                    break;
                }
            }
        }

        // if not found, lookup via public address if possible
        log_info("sm peer addr type %u, peer addres %s", setup->sm_peer_addr_type, bd_addr_to_str(setup->sm_peer_address));
        if (le_db_index < 0 && setup->sm_peer_addr_type == BD_ADDR_TYPE_LE_PUBLIC){
            int i;
            for (i=0; i < le_device_db_max_count(); i++){
                bd_addr_t address;
                int address_type;
                le_device_db_info(i, &address_type, address, NULL);
                log_info("device %u, sm peer addr type %u, peer addres %s", i, address_type, bd_addr_to_str(address));
                if (address_type == BD_ADDR_TYPE_LE_PUBLIC && memcmp(address, setup->sm_peer_address, 6) == 0){
                    log_info("sm: device found for public address, updating");
                    le_db_index = i;
                    break;
                }
            }
        }

        // if not found, add to db
        if (le_db_index < 0) {
            le_db_index = le_device_db_add(setup->sm_peer_addr_type, setup->sm_peer_address, setup->sm_peer_irk);
        }

        if (le_db_index >= 0){

            sm_notify_client_index(SM_EVENT_IDENTITY_CREATED, sm_conn->sm_handle, setup->sm_peer_addr_type, setup->sm_peer_address, le_db_index);

#ifdef ENABLE_LE_SIGNED_WRITE
            // store local CSRK
            if (setup->sm_key_distribution_send_set & SM_KEYDIST_FLAG_SIGNING_IDENTIFICATION){
                log_info("sm: store local CSRK");
                le_device_db_local_csrk_set(le_db_index, setup->sm_local_csrk);
                le_device_db_local_counter_set(le_db_index, 0);
            }

            // store remote CSRK
            if (setup->sm_key_distribution_received_set & SM_KEYDIST_FLAG_SIGNING_IDENTIFICATION){
                log_info("sm: store remote CSRK");
                le_device_db_remote_csrk_set(le_db_index, setup->sm_peer_csrk);
                le_device_db_remote_counter_set(le_db_index, 0);
            }
#endif
            // store encryption information for secure connections: LTK generated by ECDH
            if (setup->sm_use_secure_connections){
                log_info("sm: store SC LTK (key size %u, authenticated %u)", sm_conn->sm_actual_encryption_key_size, sm_conn->sm_connection_authenticated);
                uint8_t zero_rand[8];
                memset(zero_rand, 0, 8);
                le_device_db_encryption_set(le_db_index, 0, zero_rand, setup->sm_ltk, sm_conn->sm_actual_encryption_key_size,
                    sm_conn->sm_connection_authenticated, sm_conn->sm_connection_authorization_state == AUTHORIZATION_GRANTED);
            }

            // store encryption information for legacy pairing: peer LTK, EDIV, RAND
            else if ( (setup->sm_key_distribution_received_set & SM_KEYDIST_FLAG_ENCRYPTION_INFORMATION)
                   && (setup->sm_key_distribution_received_set & SM_KEYDIST_FLAG_MASTER_IDENTIFICATION )){
                log_info("sm: set encryption information (key size %u, authenticated %u)", sm_conn->sm_actual_encryption_key_size, sm_conn->sm_connection_authenticated);
                le_device_db_encryption_set(le_db_index, setup->sm_peer_ediv, setup->sm_peer_rand, setup->sm_peer_ltk,
                    sm_conn->sm_actual_encryption_key_size, sm_conn->sm_connection_authenticated, sm_conn->sm_connection_authorization_state == AUTHORIZATION_GRANTED);

            }
        }
    } else {
        log_info("Ignoring received keys, bonding not enabled");
    }

    // keep le_db_index
    sm_conn->sm_le_db_index = le_db_index;
}

static void sm_pairing_error(sm_connection_t * sm_conn, uint8_t reason){
    setup->sm_pairing_failed_reason = reason;
    sm_conn->sm_engine_state = SM_GENERAL_SEND_PAIRING_FAILED;
}

static inline void sm_pdu_received_in_wrong_state(sm_connection_t * sm_conn){
    sm_pairing_error(sm_conn, SM_REASON_UNSPECIFIED_REASON);
}

#ifdef ENABLE_LE_SECURE_CONNECTIONS

static void sm_sc_prepare_dhkey_check(sm_connection_t * sm_conn);
static int sm_passkey_used(stk_generation_method_t method);
static int sm_just_works_or_numeric_comparison(stk_generation_method_t method);

static void sm_sc_start_calculating_local_confirm(sm_connection_t * sm_conn){
    if (sm_passkey_used(setup->sm_stk_generation_method)){
        // sm_conn->sm_engine_state = SM_SC_W2_GET_RANDOM_A;
        btstack_crypto_random_generate(&sm_crypto_random_request, setup->sm_local_nonce, 16, &sm_handle_random_result_sc_get_random, sm_conn);
    } else {
        sm_conn->sm_engine_state = SM_SC_W2_CMAC_FOR_CONFIRMATION;
    }
}

static void sm_sc_state_after_receiving_random(sm_connection_t * sm_conn){
    if (IS_RESPONDER(sm_conn->sm_role)){
        // Responder
        if (setup->sm_stk_generation_method == OOB){
            // generate Nb
            log_info("Generate Nb");
            // sm_conn->sm_engine_state = SM_SC_W2_GET_RANDOM_A;
            btstack_crypto_random_generate(&sm_crypto_random_request, setup->sm_local_nonce, 16, &sm_handle_random_result_sc_get_random, sm_conn);
        } else {
            sm_conn->sm_engine_state = SM_SC_SEND_PAIRING_RANDOM;
        }
    } else {
        // Initiator role
        switch (setup->sm_stk_generation_method){
            case JUST_WORKS:
                sm_sc_prepare_dhkey_check(sm_conn);
                break;

            case NUMERIC_COMPARISON:
                sm_conn->sm_engine_state = SM_SC_W2_CALCULATE_G2;
                break;
            case PK_INIT_INPUT:
            case PK_RESP_INPUT:
            case PK_BOTH_INPUT:
                if (setup->sm_passkey_bit < 20) {
                    sm_sc_start_calculating_local_confirm(sm_conn);
                } else {
                    sm_sc_prepare_dhkey_check(sm_conn);
                }
                break;
            case OOB:
                sm_sc_prepare_dhkey_check(sm_conn);
                break;
        }
    }
}

static void sm_sc_cmac_done(uint8_t * hash){
    log_info("sm_sc_cmac_done: ");
    log_info_hexdump(hash, 16);

    if (sm_sc_oob_state == SM_SC_OOB_W4_CONFIRM){
        sm_sc_oob_state = SM_SC_OOB_IDLE;
        (*sm_sc_oob_callback)(hash, sm_sc_oob_random);
        return;
    }

    sm_connection_t * sm_conn = sm_cmac_connection;
    sm_cmac_connection = NULL;
#ifdef ENABLE_CLASSIC
    link_key_type_t link_key_type;
#endif

    switch (sm_conn->sm_engine_state){
        case SM_SC_W4_CMAC_FOR_CONFIRMATION:
            memcpy(setup->sm_local_confirm, hash, 16);
            sm_conn->sm_engine_state = SM_SC_SEND_CONFIRMATION;
            break;
        case SM_SC_W4_CMAC_FOR_CHECK_CONFIRMATION:
            // check
            if (0 != memcmp(hash, setup->sm_peer_confirm, 16)){
                sm_pairing_error(sm_conn, SM_REASON_CONFIRM_VALUE_FAILED);
                break;
            }
            sm_sc_state_after_receiving_random(sm_conn);
            break;
        case SM_SC_W4_CALCULATE_G2: {
            uint32_t vab = big_endian_read_32(hash, 12) % 1000000;
            big_endian_store_32(setup->sm_tk, 12, vab);
            sm_conn->sm_engine_state = SM_SC_W4_USER_RESPONSE;
            sm_trigger_user_response(sm_conn);
            break;
        }
        case SM_SC_W4_CALCULATE_F5_SALT:
            memcpy(setup->sm_t, hash, 16);
            sm_conn->sm_engine_state = SM_SC_W2_CALCULATE_F5_MACKEY;
            break;
        case SM_SC_W4_CALCULATE_F5_MACKEY:
            memcpy(setup->sm_mackey, hash, 16);
            sm_conn->sm_engine_state = SM_SC_W2_CALCULATE_F5_LTK;
            break;
        case SM_SC_W4_CALCULATE_F5_LTK:
            // truncate sm_ltk, but keep full LTK for cross-transport key derivation in sm_local_ltk
            // Errata Service Release to the Bluetooth Specification: ESR09
            //   E6405 – Cross transport key derivation from a key of size less than 128 bits
            //   Note: When the BR/EDR link key is being derived from the LTK, the derivation is done before the LTK gets masked."
            memcpy(setup->sm_ltk, hash, 16);
            memcpy(setup->sm_local_ltk, hash, 16);
            sm_truncate_key(setup->sm_ltk, sm_conn->sm_actual_encryption_key_size);
            sm_conn->sm_engine_state = SM_SC_W2_CALCULATE_F6_FOR_DHKEY_CHECK;
            break;
        case SM_SC_W4_CALCULATE_F6_FOR_DHKEY_CHECK:
            memcpy(setup->sm_local_dhkey_check, hash, 16);
            if (IS_RESPONDER(sm_conn->sm_role)){
                // responder
                if (setup->sm_state_vars & SM_STATE_VAR_DHKEY_COMMAND_RECEIVED){
                    sm_conn->sm_engine_state = SM_SC_W2_CALCULATE_F6_TO_VERIFY_DHKEY_CHECK;
                } else {
                    sm_conn->sm_engine_state = SM_SC_W4_DHKEY_CHECK_COMMAND;
                }
            } else {
                sm_conn->sm_engine_state = SM_SC_SEND_DHKEY_CHECK_COMMAND;
            }
            break;
        case SM_SC_W4_CALCULATE_F6_TO_VERIFY_DHKEY_CHECK:
            if (0 != memcmp(hash, setup->sm_peer_dhkey_check, 16) ){
                sm_pairing_error(sm_conn, SM_REASON_DHKEY_CHECK_FAILED);
                break;
            }
            if (IS_RESPONDER(sm_conn->sm_role)){
                // responder
                sm_conn->sm_engine_state = SM_SC_SEND_DHKEY_CHECK_COMMAND;
            } else {
                // initiator
                sm_conn->sm_engine_state = SM_INITIATOR_PH3_SEND_START_ENCRYPTION;
            }
            break;
        case SM_SC_W4_CALCULATE_H6_ILK:
            memcpy(setup->sm_t, hash, 16);
            sm_conn->sm_engine_state = SM_SC_W2_CALCULATE_H6_BR_EDR_LINK_KEY;
            break;
        case SM_SC_W4_CALCULATE_H6_BR_EDR_LINK_KEY:
#ifdef ENABLE_CLASSIC
            reverse_128(hash, setup->sm_t);
            link_key_type = sm_conn->sm_connection_authenticated ?
                AUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256 : UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256;
            log_info("Derived classic link key from LE using h6, type %u", (int) link_key_type);
            if (IS_RESPONDER(sm_conn->sm_role)){
                gap_store_link_key_for_bd_addr(setup->sm_m_address, setup->sm_t, link_key_type);
            } else {
                gap_store_link_key_for_bd_addr(setup->sm_s_address, setup->sm_t, link_key_type);
            }
#endif
            if (IS_RESPONDER(sm_conn->sm_role)){
                sm_conn->sm_engine_state = SM_RESPONDER_IDLE;
            } else {
                sm_conn->sm_engine_state = SM_INITIATOR_CONNECTED;
            }
            sm_notify_client_status_reason(sm_conn, ERROR_CODE_SUCCESS, 0);
            sm_done_for_handle(sm_conn->sm_handle);
            break;
        default:
            log_error("sm_sc_cmac_done in state %u", sm_conn->sm_engine_state);
            break;
    }
    sm_run();
}

static void f4_engine(sm_connection_t * sm_conn, const sm_key256_t u, const sm_key256_t v, const sm_key_t x, uint8_t z){
    const uint16_t message_len = 65;
    sm_cmac_connection = sm_conn;
    memcpy(sm_cmac_sc_buffer, u, 32);
    memcpy(sm_cmac_sc_buffer+32, v, 32);
    sm_cmac_sc_buffer[64] = z;
    log_info("f4 key");
    log_info_hexdump(x, 16);
    log_info("f4 message");
    log_info_hexdump(sm_cmac_sc_buffer, message_len);
    sm_cmac_message_start(x, message_len, sm_cmac_sc_buffer, &sm_sc_cmac_done);
}

static const sm_key_t f5_salt = { 0x6C ,0x88, 0x83, 0x91, 0xAA, 0xF5, 0xA5, 0x38, 0x60, 0x37, 0x0B, 0xDB, 0x5A, 0x60, 0x83, 0xBE};
static const uint8_t f5_key_id[] = { 0x62, 0x74, 0x6c, 0x65 };
static const uint8_t f5_length[] = { 0x01, 0x00};

static void f5_calculate_salt(sm_connection_t * sm_conn){
    log_info("f5_calculate_salt");
    // calculate salt for f5
    const uint16_t message_len = 32;
    sm_cmac_connection = sm_conn;
    memcpy(sm_cmac_sc_buffer, setup->sm_dhkey, message_len);
    sm_cmac_message_start(f5_salt, message_len, sm_cmac_sc_buffer, &sm_sc_cmac_done);
}

static inline void f5_mackkey(sm_connection_t * sm_conn, sm_key_t t, const sm_key_t n1, const sm_key_t n2, const sm_key56_t a1, const sm_key56_t a2){
    const uint16_t message_len = 53;
    sm_cmac_connection = sm_conn;

    // f5(W, N1, N2, A1, A2) = AES-CMACT (Counter = 0 || keyID || N1 || N2|| A1|| A2 || Length = 256) -- this is the MacKey
    sm_cmac_sc_buffer[0] = 0;
    memcpy(sm_cmac_sc_buffer+01, f5_key_id, 4);
    memcpy(sm_cmac_sc_buffer+05, n1, 16);
    memcpy(sm_cmac_sc_buffer+21, n2, 16);
    memcpy(sm_cmac_sc_buffer+37, a1, 7);
    memcpy(sm_cmac_sc_buffer+44, a2, 7);
    memcpy(sm_cmac_sc_buffer+51, f5_length, 2);
    log_info("f5 key");
    log_info_hexdump(t, 16);
    log_info("f5 message for MacKey");
    log_info_hexdump(sm_cmac_sc_buffer, message_len);
    sm_cmac_message_start(t, message_len, sm_cmac_sc_buffer, &sm_sc_cmac_done);
}

static void f5_calculate_mackey(sm_connection_t * sm_conn){
    sm_key56_t bd_addr_master, bd_addr_slave;
    bd_addr_master[0] =  setup->sm_m_addr_type;
    bd_addr_slave[0]  =  setup->sm_s_addr_type;
    memcpy(&bd_addr_master[1], setup->sm_m_address, 6);
    memcpy(&bd_addr_slave[1],  setup->sm_s_address, 6);
    if (IS_RESPONDER(sm_conn->sm_role)){
        // responder
        f5_mackkey(sm_conn, setup->sm_t, setup->sm_peer_nonce, setup->sm_local_nonce, bd_addr_master, bd_addr_slave);
    } else {
        // initiator
        f5_mackkey(sm_conn, setup->sm_t, setup->sm_local_nonce, setup->sm_peer_nonce, bd_addr_master, bd_addr_slave);
    }
}

// note: must be called right after f5_mackey, as sm_cmac_buffer[1..52] will be reused
static inline void f5_ltk(sm_connection_t * sm_conn, sm_key_t t){
    const uint16_t message_len = 53;
    sm_cmac_connection = sm_conn;
    sm_cmac_sc_buffer[0] = 1;
    // 1..52 setup before
    log_info("f5 key");
    log_info_hexdump(t, 16);
    log_info("f5 message for LTK");
    log_info_hexdump(sm_cmac_sc_buffer, message_len);
    sm_cmac_message_start(t, message_len, sm_cmac_sc_buffer, &sm_sc_cmac_done);
}

static void f5_calculate_ltk(sm_connection_t * sm_conn){
    f5_ltk(sm_conn, setup->sm_t);
}

static void f6_engine(sm_connection_t * sm_conn, const sm_key_t w, const sm_key_t n1, const sm_key_t n2, const sm_key_t r, const sm_key24_t io_cap, const sm_key56_t a1, const sm_key56_t a2){
    const uint16_t message_len = 65;
    sm_cmac_connection = sm_conn;
    memcpy(sm_cmac_sc_buffer, n1, 16);
    memcpy(sm_cmac_sc_buffer+16, n2, 16);
    memcpy(sm_cmac_sc_buffer+32, r, 16);
    memcpy(sm_cmac_sc_buffer+48, io_cap, 3);
    memcpy(sm_cmac_sc_buffer+51, a1, 7);
    memcpy(sm_cmac_sc_buffer+58, a2, 7);
    log_info("f6 key");
    log_info_hexdump(w, 16);
    log_info("f6 message");
    log_info_hexdump(sm_cmac_sc_buffer, message_len);
    sm_cmac_message_start(w, 65, sm_cmac_sc_buffer, &sm_sc_cmac_done);
}

// g2(U, V, X, Y) = AES-CMACX(U || V || Y) mod 2^32
// - U is 256 bits
// - V is 256 bits
// - X is 128 bits
// - Y is 128 bits
static void g2_engine(sm_connection_t * sm_conn, const sm_key256_t u, const sm_key256_t v, const sm_key_t x, const sm_key_t y){
    const uint16_t message_len = 80;
    sm_cmac_connection = sm_conn;
    memcpy(sm_cmac_sc_buffer, u, 32);
    memcpy(sm_cmac_sc_buffer+32, v, 32);
    memcpy(sm_cmac_sc_buffer+64, y, 16);
    log_info("g2 key");
    log_info_hexdump(x, 16);
    log_info("g2 message");
    log_info_hexdump(sm_cmac_sc_buffer, message_len);
    sm_cmac_message_start(x, message_len, sm_cmac_sc_buffer, &sm_sc_cmac_done);
}

static void g2_calculate(sm_connection_t * sm_conn) {
    // calc Va if numeric comparison
    if (IS_RESPONDER(sm_conn->sm_role)){
        // responder
        g2_engine(sm_conn, setup->sm_peer_q, ec_q, setup->sm_peer_nonce, setup->sm_local_nonce);;
    } else {
        // initiator
        g2_engine(sm_conn, ec_q, setup->sm_peer_q, setup->sm_local_nonce, setup->sm_peer_nonce);
    }
}

static void sm_sc_calculate_local_confirm(sm_connection_t * sm_conn){
    uint8_t z = 0;
    if (sm_passkey_entry(setup->sm_stk_generation_method)){
        // some form of passkey
        uint32_t pk = big_endian_read_32(setup->sm_tk, 12);
        z = 0x80 | ((pk >> setup->sm_passkey_bit) & 1);
        setup->sm_passkey_bit++;
    }
    f4_engine(sm_conn, ec_q, setup->sm_peer_q, setup->sm_local_nonce, z);
}

static void sm_sc_calculate_remote_confirm(sm_connection_t * sm_conn){
    // OOB
    if (setup->sm_stk_generation_method == OOB){
        if (IS_RESPONDER(sm_conn->sm_role)){
            f4_engine(sm_conn, setup->sm_peer_q, setup->sm_peer_q, setup->sm_ra, 0);
        } else {
            f4_engine(sm_conn, setup->sm_peer_q, setup->sm_peer_q, setup->sm_rb, 0);
        }
        return;
    }

    uint8_t z = 0;
    if (sm_passkey_entry(setup->sm_stk_generation_method)){
        // some form of passkey
        uint32_t pk = big_endian_read_32(setup->sm_tk, 12);
        // sm_passkey_bit was increased before sending confirm value
        z = 0x80 | ((pk >> (setup->sm_passkey_bit-1)) & 1);
    }
    f4_engine(sm_conn, setup->sm_peer_q, ec_q, setup->sm_peer_nonce, z);
}

static void sm_sc_prepare_dhkey_check(sm_connection_t * sm_conn){
    log_info("sm_sc_prepare_dhkey_check, DHKEY calculated %u", setup->sm_state_vars & SM_STATE_VAR_DHKEY_CALCULATED ? 1 : 0);

    if (setup->sm_state_vars & SM_STATE_VAR_DHKEY_CALCULATED){
        sm_conn->sm_engine_state = SM_SC_W2_CALCULATE_F5_SALT;
        return;
    } else {
        sm_conn->sm_engine_state = SM_SC_W4_CALCULATE_DHKEY;
    }
}

static void sm_sc_dhkey_calculated(void * arg){
    sm_connection_t * sm_conn = (sm_connection_t *) arg;
    log_info("dhkey");
    log_info_hexdump(&setup->sm_dhkey[0], 32);
    setup->sm_state_vars |= SM_STATE_VAR_DHKEY_CALCULATED;
    // trigger next step
    if (sm_conn->sm_engine_state == SM_SC_W4_CALCULATE_DHKEY){
        sm_conn->sm_engine_state = SM_SC_W2_CALCULATE_F5_SALT;
    }
    sm_run();
}

static void sm_sc_calculate_f6_for_dhkey_check(sm_connection_t * sm_conn){
    // calculate DHKCheck
    sm_key56_t bd_addr_master, bd_addr_slave;
    bd_addr_master[0] =  setup->sm_m_addr_type;
    bd_addr_slave[0]  =  setup->sm_s_addr_type;
    memcpy(&bd_addr_master[1], setup->sm_m_address, 6);
    memcpy(&bd_addr_slave[1],  setup->sm_s_address, 6);
    uint8_t iocap_a[3];
    iocap_a[0] = sm_pairing_packet_get_auth_req(setup->sm_m_preq);
    iocap_a[1] = sm_pairing_packet_get_oob_data_flag(setup->sm_m_preq);
    iocap_a[2] = sm_pairing_packet_get_io_capability(setup->sm_m_preq);
    uint8_t iocap_b[3];
    iocap_b[0] = sm_pairing_packet_get_auth_req(setup->sm_s_pres);
    iocap_b[1] = sm_pairing_packet_get_oob_data_flag(setup->sm_s_pres);
    iocap_b[2] = sm_pairing_packet_get_io_capability(setup->sm_s_pres);
    if (IS_RESPONDER(sm_conn->sm_role)){
        // responder
        f6_engine(sm_conn, setup->sm_mackey, setup->sm_local_nonce, setup->sm_peer_nonce, setup->sm_ra, iocap_b, bd_addr_slave, bd_addr_master);
    } else {
        // initiator
        f6_engine(sm_conn, setup->sm_mackey, setup->sm_local_nonce, setup->sm_peer_nonce, setup->sm_rb, iocap_a, bd_addr_master, bd_addr_slave);
    }
}

static void sm_sc_calculate_f6_to_verify_dhkey_check(sm_connection_t * sm_conn){
    // validate E = f6()
    sm_key56_t bd_addr_master, bd_addr_slave;
    bd_addr_master[0] =  setup->sm_m_addr_type;
    bd_addr_slave[0]  =  setup->sm_s_addr_type;
    memcpy(&bd_addr_master[1], setup->sm_m_address, 6);
    memcpy(&bd_addr_slave[1],  setup->sm_s_address, 6);

    uint8_t iocap_a[3];
    iocap_a[0] = sm_pairing_packet_get_auth_req(setup->sm_m_preq);
    iocap_a[1] = sm_pairing_packet_get_oob_data_flag(setup->sm_m_preq);
    iocap_a[2] = sm_pairing_packet_get_io_capability(setup->sm_m_preq);
    uint8_t iocap_b[3];
    iocap_b[0] = sm_pairing_packet_get_auth_req(setup->sm_s_pres);
    iocap_b[1] = sm_pairing_packet_get_oob_data_flag(setup->sm_s_pres);
    iocap_b[2] = sm_pairing_packet_get_io_capability(setup->sm_s_pres);
    if (IS_RESPONDER(sm_conn->sm_role)){
        // responder
        f6_engine(sm_conn, setup->sm_mackey, setup->sm_peer_nonce, setup->sm_local_nonce, setup->sm_rb, iocap_a, bd_addr_master, bd_addr_slave);
    } else {
        // initiator
        f6_engine(sm_conn, setup->sm_mackey, setup->sm_peer_nonce, setup->sm_local_nonce, setup->sm_ra, iocap_b, bd_addr_slave, bd_addr_master);
    }
}


//
// Link Key Conversion Function h6
//
// h6(W, keyID) = AES-CMACW(keyID)
// - W is 128 bits
// - keyID is 32 bits
static void h6_engine(sm_connection_t * sm_conn, const sm_key_t w, const uint32_t key_id){
    const uint16_t message_len = 4;
    sm_cmac_connection = sm_conn;
    big_endian_store_32(sm_cmac_sc_buffer, 0, key_id);
    log_info("h6 key");
    log_info_hexdump(w, 16);
    log_info("h6 message");
    log_info_hexdump(sm_cmac_sc_buffer, message_len);
    sm_cmac_message_start(w, message_len, sm_cmac_sc_buffer, &sm_sc_cmac_done);
}

// For SC, setup->sm_local_ltk holds full LTK (sm_ltk is already truncated)
// Errata Service Release to the Bluetooth Specification: ESR09
//   E6405 – Cross transport key derivation from a key of size less than 128 bits
//   "Note: When the BR/EDR link key is being derived from the LTK, the derivation is done before the LTK gets masked."
static void h6_calculate_ilk(sm_connection_t * sm_conn){
    h6_engine(sm_conn, setup->sm_local_ltk, 0x746D7031);    // "tmp1"
}

static void h6_calculate_br_edr_link_key(sm_connection_t * sm_conn){
    h6_engine(sm_conn, setup->sm_t, 0x6c656272);    // "lebr"
}

#endif

// key management legacy connections:
// - potentially two different LTKs based on direction. each device stores LTK provided by peer
// - master stores LTK, EDIV, RAND. responder optionally stored master LTK (only if it needs to reconnect)
// - initiators reconnects: initiator uses stored LTK, EDIV, RAND generated by responder
// - responder  reconnects: responder uses LTK receveived from master

// key management secure connections:
// - both devices store same LTK from ECDH key exchange.

#if defined(ENABLE_LE_SECURE_CONNECTIONS) || defined(ENABLE_LE_CENTRAL)
static void sm_load_security_info(sm_connection_t * sm_connection){
    int encryption_key_size;
    int authenticated;
    int authorized;

    // fetch data from device db - incl. authenticated/authorized/key size. Note all sm_connection_X require encryption enabled
    le_device_db_encryption_get(sm_connection->sm_le_db_index, &setup->sm_peer_ediv, setup->sm_peer_rand, setup->sm_peer_ltk,
                                &encryption_key_size, &authenticated, &authorized);
    log_info("db index %u, key size %u, authenticated %u, authorized %u", sm_connection->sm_le_db_index, encryption_key_size, authenticated, authorized);
    sm_connection->sm_actual_encryption_key_size = encryption_key_size;
    sm_connection->sm_connection_authenticated = authenticated;
    sm_connection->sm_connection_authorization_state = authorized ? AUTHORIZATION_GRANTED : AUTHORIZATION_UNKNOWN;
}
#endif

#ifdef ENABLE_LE_PERIPHERAL
static void sm_start_calculating_ltk_from_ediv_and_rand(sm_connection_t * sm_connection){
    memcpy(setup->sm_local_rand, sm_connection->sm_local_rand, 8);
    setup->sm_local_ediv = sm_connection->sm_local_ediv;
    // re-establish used key encryption size
    // no db for encryption size hack: encryption size is stored in lowest nibble of setup->sm_local_rand
    sm_connection->sm_actual_encryption_key_size = (setup->sm_local_rand[7] & 0x0f) + 1;
    // no db for authenticated flag hack: flag is stored in bit 4 of LSB
    sm_connection->sm_connection_authenticated = (setup->sm_local_rand[7] & 0x10) >> 4;
    log_info("sm: received ltk request with key size %u, authenticated %u",
            sm_connection->sm_actual_encryption_key_size, sm_connection->sm_connection_authenticated);
    sm_connection->sm_engine_state = SM_RESPONDER_PH4_Y_GET_ENC;
    sm_run();
}
#endif

static void sm_run(void){

    btstack_linked_list_iterator_t it;

    // assert that stack has already bootet
    if (hci_get_state() != HCI_STATE_WORKING) return;

    // assert that we can send at least commands
    if (!hci_can_send_command_packet_now()) return;

    //
    // non-connection related behaviour
    //

    // distributed key generation
    switch (dkg_state){
        case DKG_CALC_IRK:
            // already busy?
            if (sm_aes128_state == SM_AES128_IDLE) {
                log_info("DKG_CALC_IRK started");
                // IRK = d1(IR, 1, 0)
                sm_d1_d_prime(1, 0, sm_aes128_plaintext);  // plaintext = d1 prime
                sm_aes128_state = SM_AES128_ACTIVE;
                btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, sm_persistent_ir, sm_aes128_plaintext, sm_persistent_irk, sm_handle_encryption_result_dkg_irk, NULL);
                return;
            }
            break;
        case DKG_CALC_DHK:
            // already busy?
            if (sm_aes128_state == SM_AES128_IDLE) {
                log_info("DKG_CALC_DHK started");
                // DHK = d1(IR, 3, 0)
                sm_d1_d_prime(3, 0, sm_aes128_plaintext);  // plaintext = d1 prime
                sm_aes128_state = SM_AES128_ACTIVE;
                btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, sm_persistent_ir, sm_aes128_plaintext, sm_persistent_dhk, sm_handle_encryption_result_dkg_dhk, NULL);
                return;
            }
            break;
        default:
            break;
    }

    // random address updates
    switch (rau_state){
        case RAU_GET_ENC:
            // already busy?
            if (sm_aes128_state == SM_AES128_IDLE) {
                sm_ah_r_prime(sm_random_address, sm_aes128_plaintext);
                sm_aes128_state = SM_AES128_ACTIVE;
                btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, sm_persistent_irk, sm_aes128_plaintext, sm_aes128_ciphertext, sm_handle_encryption_result_rau, NULL);
                return;
            }
            break;
        case RAU_SET_ADDRESS:
            log_info("New random address: %s", bd_addr_to_str(sm_random_address));
            rau_state = RAU_IDLE;
            hci_send_cmd(&hci_le_set_random_address, sm_random_address);
            return;
        default:
            break;
    }

    // CSRK Lookup
    // -- if csrk lookup ready, find connection that require csrk lookup
    if (sm_address_resolution_idle()){
        hci_connections_get_iterator(&it);
        while(btstack_linked_list_iterator_has_next(&it)){
            hci_connection_t * hci_connection = (hci_connection_t *) btstack_linked_list_iterator_next(&it);
            sm_connection_t  * sm_connection  = &hci_connection->sm_connection;
            if (sm_connection->sm_irk_lookup_state == IRK_LOOKUP_W4_READY){
                // and start lookup
                sm_address_resolution_start_lookup(sm_connection->sm_peer_addr_type, sm_connection->sm_handle, sm_connection->sm_peer_address, ADDRESS_RESOLUTION_FOR_CONNECTION, sm_connection);
                sm_connection->sm_irk_lookup_state = IRK_LOOKUP_STARTED;
                break;
            }
        }
    }

    // -- if csrk lookup ready, resolved addresses for received addresses
    if (sm_address_resolution_idle()) {
        if (!btstack_linked_list_empty(&sm_address_resolution_general_queue)){
            sm_lookup_entry_t * entry = (sm_lookup_entry_t *) sm_address_resolution_general_queue;
            btstack_linked_list_remove(&sm_address_resolution_general_queue, (btstack_linked_item_t *) entry);
            sm_address_resolution_start_lookup(entry->address_type, 0, entry->address, ADDRESS_RESOLUTION_GENERAL, NULL);
            btstack_memory_sm_lookup_entry_free(entry);
        }
    }

    // -- Continue with CSRK device lookup by public or resolvable private address
    if (!sm_address_resolution_idle()){
        log_info("LE Device Lookup: device %u/%u", sm_address_resolution_test, le_device_db_max_count());
        while (sm_address_resolution_test < le_device_db_max_count()){
            int addr_type;
            bd_addr_t addr;
            sm_key_t irk;
            le_device_db_info(sm_address_resolution_test, &addr_type, addr, irk);
            log_info("device type %u, addr: %s", addr_type, bd_addr_to_str(addr));

            if (sm_address_resolution_addr_type == addr_type && memcmp(addr, sm_address_resolution_address, 6) == 0){
                log_info("LE Device Lookup: found CSRK by { addr_type, address} ");
                sm_address_resolution_handle_event(ADDRESS_RESOLUTION_SUCEEDED);
                break;
            }

            if (sm_address_resolution_addr_type == 0){
                sm_address_resolution_test++;
                continue;
            }

            if (sm_aes128_state == SM_AES128_ACTIVE) break;

            log_info("LE Device Lookup: calculate AH");
            log_info_key("IRK", irk);

            memcpy(sm_aes128_key, irk, 16);
            sm_ah_r_prime(sm_address_resolution_address, sm_aes128_plaintext);
            sm_address_resolution_ah_calculation_active = 1;
            sm_aes128_state = SM_AES128_ACTIVE;
            btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, sm_aes128_key, sm_aes128_plaintext, sm_aes128_ciphertext, sm_handle_encryption_result_address_resolution, NULL);
            return;
        }

        if (sm_address_resolution_test >= le_device_db_max_count()){
            log_info("LE Device Lookup: not found");
            sm_address_resolution_handle_event(ADDRESS_RESOLUTION_FAILED);
        }
    }

#ifdef ENABLE_LE_SECURE_CONNECTIONS
    switch (sm_sc_oob_state){
        case SM_SC_OOB_W2_CALC_CONFIRM:
            if (!sm_cmac_ready()) break;
            sm_sc_oob_state = SM_SC_OOB_W4_CONFIRM;
            f4_engine(NULL, ec_q, ec_q, sm_sc_oob_random, 0);
            return;
        default:
            break;
    }
#endif

    // assert that we can send at least commands - cmd might have been sent by crypto engine
    if (!hci_can_send_command_packet_now()) return;

    // handle basic actions that don't requires the full context
    hci_connections_get_iterator(&it);
    while((sm_active_connection_handle == HCI_CON_HANDLE_INVALID) && btstack_linked_list_iterator_has_next(&it)){
        hci_connection_t * hci_connection = (hci_connection_t *) btstack_linked_list_iterator_next(&it);
        sm_connection_t  * sm_connection = &hci_connection->sm_connection;
        switch(sm_connection->sm_engine_state){
            // responder side
            case SM_RESPONDER_PH0_SEND_LTK_REQUESTED_NEGATIVE_REPLY:
                sm_connection->sm_engine_state = SM_RESPONDER_IDLE;
                hci_send_cmd(&hci_le_long_term_key_negative_reply, sm_connection->sm_handle);
                return;

#ifdef ENABLE_LE_SECURE_CONNECTIONS
            case SM_SC_RECEIVED_LTK_REQUEST:
                switch (sm_connection->sm_irk_lookup_state){
                    case IRK_LOOKUP_FAILED:
                        log_info("LTK Request: ediv & random are empty, but no stored LTK (IRK Lookup Failed)");
                        sm_connection->sm_engine_state = SM_RESPONDER_IDLE;
                        hci_send_cmd(&hci_le_long_term_key_negative_reply, sm_connection->sm_handle);
                        return;
                    default:
                        break;
                }
                break;
#endif
            default:
                break;
        }
    }

    //
    // active connection handling
    // -- use loop to handle next connection if lock on setup context is released

    while (1) {

        // Find connections that requires setup context and make active if no other is locked
        hci_connections_get_iterator(&it);
        while((sm_active_connection_handle == HCI_CON_HANDLE_INVALID) && btstack_linked_list_iterator_has_next(&it)){
            hci_connection_t * hci_connection = (hci_connection_t *) btstack_linked_list_iterator_next(&it);
            sm_connection_t  * sm_connection = &hci_connection->sm_connection;
            // - if no connection locked and we're ready/waiting for setup context, fetch it and start
            int done = 1;
            int err;
            UNUSED(err);
            switch (sm_connection->sm_engine_state) {
#ifdef ENABLE_LE_PERIPHERAL
                case SM_RESPONDER_SEND_SECURITY_REQUEST:
                    // send packet if possible,
                    if (l2cap_can_send_fixed_channel_packet_now(sm_connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL)){
                        const uint8_t buffer[2] = { SM_CODE_SECURITY_REQUEST, SM_AUTHREQ_BONDING};
                        sm_connection->sm_engine_state = SM_RESPONDER_PH1_W4_PAIRING_REQUEST;
                        l2cap_send_connectionless(sm_connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                    } else {
                        l2cap_request_can_send_fix_channel_now_event(sm_connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL);
                    }
                    // don't lock sxetup context yet
                    done = 0;
                    break;
                case SM_RESPONDER_PH1_PAIRING_REQUEST_RECEIVED:
                    sm_reset_setup();
                    sm_init_setup(sm_connection);
                    // recover pairing request
                    memcpy(&setup->sm_m_preq, &sm_connection->sm_m_preq, sizeof(sm_pairing_packet_t));
                    err = sm_stk_generation_init(sm_connection);

#ifdef ENABLE_TESTING_SUPPORT
                    if (0 < test_pairing_failure && test_pairing_failure < SM_REASON_DHKEY_CHECK_FAILED){
                        log_info("testing_support: respond with pairing failure %u", test_pairing_failure);
                        err = test_pairing_failure;
                    }
#endif
                    if (err){
                        setup->sm_pairing_failed_reason = err;
                        sm_connection->sm_engine_state = SM_GENERAL_SEND_PAIRING_FAILED;
                        break;
                    }
                    sm_timeout_start(sm_connection);
                    // generate random number first, if we need to show passkey
                    if (setup->sm_stk_generation_method == PK_INIT_INPUT){
                        btstack_crypto_random_generate(&sm_crypto_random_request, sm_random_data, 8, &sm_handle_random_result_ph2_tk, sm_connection);
                        break;
                    }
                    sm_connection->sm_engine_state = SM_RESPONDER_PH1_SEND_PAIRING_RESPONSE;
                    break;
                case SM_RESPONDER_PH0_RECEIVED_LTK_REQUEST:
                    sm_reset_setup();
                    sm_start_calculating_ltk_from_ediv_and_rand(sm_connection);
                    break;
#endif
#ifdef ENABLE_LE_CENTRAL
                case SM_INITIATOR_PH0_HAS_LTK:
                    sm_reset_setup();
                    sm_load_security_info(sm_connection);
                    sm_connection->sm_engine_state = SM_INITIATOR_PH0_SEND_START_ENCRYPTION;
                    break;
                case SM_INITIATOR_PH1_W2_SEND_PAIRING_REQUEST:
                    sm_reset_setup();
                    sm_init_setup(sm_connection);
                    sm_timeout_start(sm_connection);
                    sm_connection->sm_engine_state = SM_INITIATOR_PH1_SEND_PAIRING_REQUEST;
                    break;
#endif

#ifdef ENABLE_LE_SECURE_CONNECTIONS
                case SM_SC_RECEIVED_LTK_REQUEST:
                    switch (sm_connection->sm_irk_lookup_state){
                        case IRK_LOOKUP_SUCCEEDED:
                            // assuming Secure Connection, we have a stored LTK and the EDIV/RAND are null
                            // start using context by loading security info
                            sm_reset_setup();
                            sm_load_security_info(sm_connection);
                            if (setup->sm_peer_ediv == 0 && sm_is_null_random(setup->sm_peer_rand) && !sm_is_null_key(setup->sm_peer_ltk)){
                                memcpy(setup->sm_ltk, setup->sm_peer_ltk, 16);
                                sm_connection->sm_engine_state = SM_RESPONDER_PH4_SEND_LTK_REPLY;
                                break;
                            }
                            log_info("LTK Request: ediv & random are empty, but no stored LTK (IRK Lookup Succeeded)");
                            sm_connection->sm_engine_state = SM_RESPONDER_IDLE;
                            hci_send_cmd(&hci_le_long_term_key_negative_reply, sm_connection->sm_handle);
                            // don't lock setup context yet
                            return;
                        default:
                            // just wait until IRK lookup is completed
                            // don't lock setup context yet
                            done = 0;
                            break;
                    }
                    break;
#endif
                default:
                    done = 0;
                    break;
            }
            if (done){
                sm_active_connection_handle = sm_connection->sm_handle;
                log_info("sm: connection 0x%04x locked setup context as %s, state %u", sm_active_connection_handle, sm_connection->sm_role ? "responder" : "initiator", sm_connection->sm_engine_state);
            }
        }

        //
        // active connection handling
        //

        if (sm_active_connection_handle == HCI_CON_HANDLE_INVALID) return;

        sm_connection_t * connection = sm_get_connection_for_handle(sm_active_connection_handle);
        if (!connection) {
            log_info("no connection for handle 0x%04x", sm_active_connection_handle);
            return;
        }

        // assert that we could send a SM PDU - not needed for all of the following
        if (!l2cap_can_send_fixed_channel_packet_now(sm_active_connection_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL)) {
            log_info("cannot send now, requesting can send now event");
            l2cap_request_can_send_fix_channel_now_event(sm_active_connection_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL);
            return;
        }

        // send keypress notifications
        if (setup->sm_keypress_notification){
            int i;
            uint8_t flags       = setup->sm_keypress_notification & 0x1f;
            uint8_t num_actions = setup->sm_keypress_notification >> 5;
            uint8_t action = 0;
            for (i=SM_KEYPRESS_PASSKEY_ENTRY_STARTED;i<=SM_KEYPRESS_PASSKEY_ENTRY_COMPLETED;i++){
                if (flags & (1<<i)){
                    int clear_flag = 1;
                    switch (i){
                        case SM_KEYPRESS_PASSKEY_ENTRY_STARTED:
                        case SM_KEYPRESS_PASSKEY_CLEARED:
                        case SM_KEYPRESS_PASSKEY_ENTRY_COMPLETED:
                        default:
                            break;
                        case SM_KEYPRESS_PASSKEY_DIGIT_ENTERED:
                        case SM_KEYPRESS_PASSKEY_DIGIT_ERASED:
                            num_actions--;
                            clear_flag = num_actions == 0;
                            break;
                    }
                    if (clear_flag){
                        flags &= ~(1<<i);
                    }
                    action = i;
                    break;
                }
            }
            setup->sm_keypress_notification = (num_actions << 5) | flags;

            // send keypress notification
            uint8_t buffer[2];
            buffer[0] = SM_CODE_KEYPRESS_NOTIFICATION;
            buffer[1] = action;
            l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));

            // try
            l2cap_request_can_send_fix_channel_now_event(sm_active_connection_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL);
            return;
        }

        int key_distribution_flags;
        UNUSED(key_distribution_flags);

        log_info("sm_run: state %u", connection->sm_engine_state);
        if (!l2cap_can_send_fixed_channel_packet_now(sm_active_connection_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL)) {
            log_info("sm_run // cannot send");
        }
        switch (connection->sm_engine_state){

            // general
            case SM_GENERAL_SEND_PAIRING_FAILED: {
                uint8_t buffer[2];
                buffer[0] = SM_CODE_PAIRING_FAILED;
                buffer[1] = setup->sm_pairing_failed_reason;
                connection->sm_engine_state = connection->sm_role ? SM_RESPONDER_IDLE : SM_INITIATOR_CONNECTED;
                l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_notify_client_status_reason(connection, ERROR_CODE_AUTHENTICATION_FAILURE, setup->sm_pairing_failed_reason);
                sm_done_for_handle(connection->sm_handle);
                break;
            }

            // responding state
#ifdef ENABLE_LE_SECURE_CONNECTIONS
            case SM_SC_W2_CMAC_FOR_CONFIRMATION:
                if (!sm_cmac_ready()) break;
                connection->sm_engine_state = SM_SC_W4_CMAC_FOR_CONFIRMATION;
                sm_sc_calculate_local_confirm(connection);
                break;
            case SM_SC_W2_CMAC_FOR_CHECK_CONFIRMATION:
                if (!sm_cmac_ready()) break;
                connection->sm_engine_state = SM_SC_W4_CMAC_FOR_CHECK_CONFIRMATION;
                sm_sc_calculate_remote_confirm(connection);
                break;
            case SM_SC_W2_CALCULATE_F6_FOR_DHKEY_CHECK:
                if (!sm_cmac_ready()) break;
                connection->sm_engine_state = SM_SC_W4_CALCULATE_F6_FOR_DHKEY_CHECK;
                sm_sc_calculate_f6_for_dhkey_check(connection);
                break;
            case SM_SC_W2_CALCULATE_F6_TO_VERIFY_DHKEY_CHECK:
                if (!sm_cmac_ready()) break;
                connection->sm_engine_state = SM_SC_W4_CALCULATE_F6_TO_VERIFY_DHKEY_CHECK;
                sm_sc_calculate_f6_to_verify_dhkey_check(connection);
                break;
            case SM_SC_W2_CALCULATE_F5_SALT:
                if (!sm_cmac_ready()) break;
                connection->sm_engine_state = SM_SC_W4_CALCULATE_F5_SALT;
                f5_calculate_salt(connection);
                break;
            case SM_SC_W2_CALCULATE_F5_MACKEY:
                if (!sm_cmac_ready()) break;
                connection->sm_engine_state = SM_SC_W4_CALCULATE_F5_MACKEY;
                f5_calculate_mackey(connection);
                break;
            case SM_SC_W2_CALCULATE_F5_LTK:
                if (!sm_cmac_ready()) break;
                connection->sm_engine_state = SM_SC_W4_CALCULATE_F5_LTK;
                f5_calculate_ltk(connection);
                break;
            case SM_SC_W2_CALCULATE_G2:
                if (!sm_cmac_ready()) break;
                connection->sm_engine_state = SM_SC_W4_CALCULATE_G2;
                g2_calculate(connection);
                break;
            case SM_SC_W2_CALCULATE_H6_ILK:
                if (!sm_cmac_ready()) break;
                connection->sm_engine_state = SM_SC_W4_CALCULATE_H6_ILK;
                h6_calculate_ilk(connection);
                break;
            case SM_SC_W2_CALCULATE_H6_BR_EDR_LINK_KEY:
                if (!sm_cmac_ready()) break;
                connection->sm_engine_state = SM_SC_W4_CALCULATE_H6_BR_EDR_LINK_KEY;
                h6_calculate_br_edr_link_key(connection);
                break;
#endif

#ifdef ENABLE_LE_CENTRAL
            // initiator side
            case SM_INITIATOR_PH0_SEND_START_ENCRYPTION: {
                sm_key_t peer_ltk_flipped;
                reverse_128(setup->sm_peer_ltk, peer_ltk_flipped);
                connection->sm_engine_state = SM_INITIATOR_PH0_W4_CONNECTION_ENCRYPTED;
                log_info("sm: hci_le_start_encryption ediv 0x%04x", setup->sm_peer_ediv);
                uint32_t rand_high = big_endian_read_32(setup->sm_peer_rand, 0);
                uint32_t rand_low  = big_endian_read_32(setup->sm_peer_rand, 4);
                hci_send_cmd(&hci_le_start_encryption, connection->sm_handle,rand_low, rand_high, setup->sm_peer_ediv, peer_ltk_flipped);
                return;
            }

            case SM_INITIATOR_PH1_SEND_PAIRING_REQUEST:
                sm_pairing_packet_set_code(setup->sm_m_preq, SM_CODE_PAIRING_REQUEST);
                connection->sm_engine_state = SM_INITIATOR_PH1_W4_PAIRING_RESPONSE;
                l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) &setup->sm_m_preq, sizeof(sm_pairing_packet_t));
                sm_timeout_reset(connection);
                break;
#endif

#ifdef ENABLE_LE_SECURE_CONNECTIONS

            case SM_SC_SEND_PUBLIC_KEY_COMMAND: {
                int trigger_user_response = 0;

                uint8_t buffer[65];
                buffer[0] = SM_CODE_PAIRING_PUBLIC_KEY;
                //
                reverse_256(&ec_q[0],  &buffer[1]);
                reverse_256(&ec_q[32], &buffer[33]);

                // stk generation method
                // passkey entry: notify app to show passkey or to request passkey
                switch (setup->sm_stk_generation_method){
                    case JUST_WORKS:
                    case NUMERIC_COMPARISON:
                        if (IS_RESPONDER(connection->sm_role)){
                            // responder
                            sm_sc_start_calculating_local_confirm(connection);
                        } else {
                            // initiator
                            connection->sm_engine_state = SM_SC_W4_PUBLIC_KEY_COMMAND;
                        }
                        break;
                    case PK_INIT_INPUT:
                    case PK_RESP_INPUT:
                    case PK_BOTH_INPUT:
                        // use random TK for display
                        memcpy(setup->sm_ra, setup->sm_tk, 16);
                        memcpy(setup->sm_rb, setup->sm_tk, 16);
                        setup->sm_passkey_bit = 0;

                        if (IS_RESPONDER(connection->sm_role)){
                            // responder
                            connection->sm_engine_state = SM_SC_W4_CONFIRMATION;
                        } else {
                            // initiator
                            connection->sm_engine_state = SM_SC_W4_PUBLIC_KEY_COMMAND;
                        }
                        trigger_user_response = 1;
                        break;
                    case OOB:
                        if (IS_RESPONDER(connection->sm_role)){
                            // responder
                            connection->sm_engine_state = SM_SC_W4_PAIRING_RANDOM;
                        } else {
                            // initiator
                            connection->sm_engine_state = SM_SC_W4_PUBLIC_KEY_COMMAND;
                        }
                        break;
                }

                l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_timeout_reset(connection);

                // trigger user response after sending pdu
                if (trigger_user_response){
                    sm_trigger_user_response(connection);
                }
                break;
            }
            case SM_SC_SEND_CONFIRMATION: {
                uint8_t buffer[17];
                buffer[0] = SM_CODE_PAIRING_CONFIRM;
                reverse_128(setup->sm_local_confirm, &buffer[1]);
                if (IS_RESPONDER(connection->sm_role)){
                    connection->sm_engine_state = SM_SC_W4_PAIRING_RANDOM;
                } else {
                    connection->sm_engine_state = SM_SC_W4_CONFIRMATION;
                }
                l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_timeout_reset(connection);
                break;
            }
            case SM_SC_SEND_PAIRING_RANDOM: {
                uint8_t buffer[17];
                buffer[0] = SM_CODE_PAIRING_RANDOM;
                reverse_128(setup->sm_local_nonce, &buffer[1]);
                log_info("stk method %u, num bits %u", setup->sm_stk_generation_method, setup->sm_passkey_bit);
                if (sm_passkey_entry(setup->sm_stk_generation_method) && setup->sm_passkey_bit < 20){
                    log_info("SM_SC_SEND_PAIRING_RANDOM A");
                    if (IS_RESPONDER(connection->sm_role)){
                        // responder
                        connection->sm_engine_state = SM_SC_W4_CONFIRMATION;
                    } else {
                        // initiator
                        connection->sm_engine_state = SM_SC_W4_PAIRING_RANDOM;
                    }
                } else {
                    log_info("SM_SC_SEND_PAIRING_RANDOM B");
                    if (IS_RESPONDER(connection->sm_role)){
                        // responder
                        if (setup->sm_stk_generation_method == NUMERIC_COMPARISON){
                            log_info("SM_SC_SEND_PAIRING_RANDOM B1");
                            connection->sm_engine_state = SM_SC_W2_CALCULATE_G2;
                        } else {
                            log_info("SM_SC_SEND_PAIRING_RANDOM B2");
                            sm_sc_prepare_dhkey_check(connection);
                        }
                    } else {
                        // initiator
                        connection->sm_engine_state = SM_SC_W4_PAIRING_RANDOM;
                    }
                }
                l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_timeout_reset(connection);
                break;
            }
            case SM_SC_SEND_DHKEY_CHECK_COMMAND: {
                uint8_t buffer[17];
                buffer[0] = SM_CODE_PAIRING_DHKEY_CHECK;
                reverse_128(setup->sm_local_dhkey_check, &buffer[1]);

                if (IS_RESPONDER(connection->sm_role)){
                    connection->sm_engine_state = SM_SC_W4_LTK_REQUEST_SC;
                } else {
                    connection->sm_engine_state = SM_SC_W4_DHKEY_CHECK_COMMAND;
                }

                l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_timeout_reset(connection);
                break;
            }

#endif

#ifdef ENABLE_LE_PERIPHERAL
            case SM_RESPONDER_PH1_SEND_PAIRING_RESPONSE:
                // echo initiator for now
                sm_pairing_packet_set_code(setup->sm_s_pres,SM_CODE_PAIRING_RESPONSE);
                key_distribution_flags = sm_key_distribution_flags_for_auth_req();

                if (setup->sm_use_secure_connections){
                    connection->sm_engine_state = SM_SC_W4_PUBLIC_KEY_COMMAND;
                } else {
                    connection->sm_engine_state = SM_RESPONDER_PH1_W4_PAIRING_CONFIRM;
                }

                sm_pairing_packet_set_initiator_key_distribution(setup->sm_s_pres, sm_pairing_packet_get_initiator_key_distribution(setup->sm_m_preq) & key_distribution_flags);
                sm_pairing_packet_set_responder_key_distribution(setup->sm_s_pres, sm_pairing_packet_get_responder_key_distribution(setup->sm_m_preq) & key_distribution_flags);
                // update key distribution after ENC was dropped
                sm_setup_key_distribution(sm_pairing_packet_get_responder_key_distribution(setup->sm_s_pres));

                l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) &setup->sm_s_pres, sizeof(sm_pairing_packet_t));
                sm_timeout_reset(connection);
                // SC Numeric Comparison will trigger user response after public keys & nonces have been exchanged
                if (!setup->sm_use_secure_connections || setup->sm_stk_generation_method == JUST_WORKS){
                    sm_trigger_user_response(connection);
                }
                return;
#endif

            case SM_PH2_SEND_PAIRING_RANDOM: {
                uint8_t buffer[17];
                buffer[0] = SM_CODE_PAIRING_RANDOM;
                reverse_128(setup->sm_local_random, &buffer[1]);
                if (IS_RESPONDER(connection->sm_role)){
                    connection->sm_engine_state = SM_RESPONDER_PH2_W4_LTK_REQUEST;
                } else {
                    connection->sm_engine_state = SM_INITIATOR_PH2_W4_PAIRING_RANDOM;
                }
                l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_timeout_reset(connection);
                break;
            }

            case SM_PH2_C1_GET_ENC_A:
                // already busy?
                if (sm_aes128_state == SM_AES128_ACTIVE) break;
                // calculate confirm using aes128 engine - step 1
                sm_c1_t1(setup->sm_local_random, (uint8_t*) &setup->sm_m_preq, (uint8_t*) &setup->sm_s_pres, setup->sm_m_addr_type, setup->sm_s_addr_type, sm_aes128_plaintext);
                connection->sm_engine_state = SM_PH2_C1_W4_ENC_A;
                sm_aes128_state = SM_AES128_ACTIVE;
                btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, setup->sm_tk, sm_aes128_plaintext, sm_aes128_ciphertext, sm_handle_encryption_result_enc_a, connection);
                break;

            case SM_PH2_C1_GET_ENC_C:
                // already busy?
                if (sm_aes128_state == SM_AES128_ACTIVE) break;
                // calculate m_confirm using aes128 engine - step 1
                sm_c1_t1(setup->sm_peer_random, (uint8_t*) &setup->sm_m_preq, (uint8_t*) &setup->sm_s_pres, setup->sm_m_addr_type, setup->sm_s_addr_type, sm_aes128_plaintext);
                connection->sm_engine_state = SM_PH2_C1_W4_ENC_C;
                sm_aes128_state = SM_AES128_ACTIVE;
                btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, setup->sm_tk, sm_aes128_plaintext, sm_aes128_ciphertext, sm_handle_encryption_result_enc_c, connection);
                break;

            case SM_PH2_CALC_STK:
                // already busy?
                if (sm_aes128_state == SM_AES128_ACTIVE) break;
                // calculate STK
                if (IS_RESPONDER(connection->sm_role)){
                    sm_s1_r_prime(setup->sm_local_random, setup->sm_peer_random, sm_aes128_plaintext);
                } else {
                    sm_s1_r_prime(setup->sm_peer_random, setup->sm_local_random, sm_aes128_plaintext);
                }
                connection->sm_engine_state = SM_PH2_W4_STK;
                sm_aes128_state = SM_AES128_ACTIVE;
                btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, setup->sm_tk, sm_aes128_plaintext, setup->sm_ltk, sm_handle_encryption_result_enc_stk, connection);
                break;

            case SM_PH3_Y_GET_ENC:
                // already busy?
                if (sm_aes128_state == SM_AES128_ACTIVE) break;
                // PH3B2 - calculate Y from      - enc
                // Y = dm(DHK, Rand)
                sm_dm_r_prime(setup->sm_local_rand, sm_aes128_plaintext);
                connection->sm_engine_state = SM_PH3_Y_W4_ENC;
                sm_aes128_state = SM_AES128_ACTIVE;
                btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, sm_persistent_dhk, sm_aes128_plaintext, sm_aes128_ciphertext, sm_handle_encryption_result_enc_ph3_y, connection);
                break;

            case SM_PH2_C1_SEND_PAIRING_CONFIRM: {
                uint8_t buffer[17];
                buffer[0] = SM_CODE_PAIRING_CONFIRM;
                reverse_128(setup->sm_local_confirm, &buffer[1]);
                if (IS_RESPONDER(connection->sm_role)){
                    connection->sm_engine_state = SM_RESPONDER_PH2_W4_PAIRING_RANDOM;
                } else {
                    connection->sm_engine_state = SM_INITIATOR_PH2_W4_PAIRING_CONFIRM;
                }
                l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_timeout_reset(connection);
                return;
            }
#ifdef ENABLE_LE_PERIPHERAL
            case SM_RESPONDER_PH2_SEND_LTK_REPLY: {
                sm_key_t stk_flipped;
                reverse_128(setup->sm_ltk, stk_flipped);
                connection->sm_engine_state = SM_PH2_W4_CONNECTION_ENCRYPTED;
                hci_send_cmd(&hci_le_long_term_key_request_reply, connection->sm_handle, stk_flipped);
                return;
            }
            case SM_RESPONDER_PH4_SEND_LTK_REPLY: {
                sm_key_t ltk_flipped;
                reverse_128(setup->sm_ltk, ltk_flipped);
                connection->sm_engine_state = SM_RESPONDER_IDLE;
                hci_send_cmd(&hci_le_long_term_key_request_reply, connection->sm_handle, ltk_flipped);
                sm_done_for_handle(connection->sm_handle);
                return;
            }
            case SM_RESPONDER_PH4_Y_GET_ENC:
                // already busy?
                if (sm_aes128_state == SM_AES128_ACTIVE) break;
                log_info("LTK Request: recalculating with ediv 0x%04x", setup->sm_local_ediv);
                // Y = dm(DHK, Rand)
                sm_dm_r_prime(setup->sm_local_rand, sm_aes128_plaintext);
                connection->sm_engine_state = SM_RESPONDER_PH4_Y_W4_ENC;
                sm_aes128_state = SM_AES128_ACTIVE;
                btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, sm_persistent_dhk, sm_aes128_plaintext, sm_aes128_ciphertext, sm_handle_encryption_result_enc_ph4_y, connection);
                return;
#endif
#ifdef ENABLE_LE_CENTRAL
            case SM_INITIATOR_PH3_SEND_START_ENCRYPTION: {
                sm_key_t stk_flipped;
                reverse_128(setup->sm_ltk, stk_flipped);
                connection->sm_engine_state = SM_PH2_W4_CONNECTION_ENCRYPTED;
                hci_send_cmd(&hci_le_start_encryption, connection->sm_handle, 0, 0, 0, stk_flipped);
                return;
            }
#endif

            case SM_PH3_DISTRIBUTE_KEYS:
                if (setup->sm_key_distribution_send_set &   SM_KEYDIST_FLAG_ENCRYPTION_INFORMATION){
                    setup->sm_key_distribution_send_set &= ~SM_KEYDIST_FLAG_ENCRYPTION_INFORMATION;
                    uint8_t buffer[17];
                    buffer[0] = SM_CODE_ENCRYPTION_INFORMATION;
                    reverse_128(setup->sm_ltk, &buffer[1]);
                    l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                    sm_timeout_reset(connection);
                    return;
                }
                if (setup->sm_key_distribution_send_set &   SM_KEYDIST_FLAG_MASTER_IDENTIFICATION){
                    setup->sm_key_distribution_send_set &= ~SM_KEYDIST_FLAG_MASTER_IDENTIFICATION;
                    uint8_t buffer[11];
                    buffer[0] = SM_CODE_MASTER_IDENTIFICATION;
                    little_endian_store_16(buffer, 1, setup->sm_local_ediv);
                    reverse_64(setup->sm_local_rand, &buffer[3]);
                    l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                    sm_timeout_reset(connection);
                    return;
                }
                if (setup->sm_key_distribution_send_set &   SM_KEYDIST_FLAG_IDENTITY_INFORMATION){
                    setup->sm_key_distribution_send_set &= ~SM_KEYDIST_FLAG_IDENTITY_INFORMATION;
                    uint8_t buffer[17];
                    buffer[0] = SM_CODE_IDENTITY_INFORMATION;
                    reverse_128(sm_persistent_irk, &buffer[1]);
                    l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                    sm_timeout_reset(connection);
                    return;
                }
                if (setup->sm_key_distribution_send_set &   SM_KEYDIST_FLAG_IDENTITY_ADDRESS_INFORMATION){
                    setup->sm_key_distribution_send_set &= ~SM_KEYDIST_FLAG_IDENTITY_ADDRESS_INFORMATION;
                    bd_addr_t local_address;
                    uint8_t buffer[8];
                    buffer[0] = SM_CODE_IDENTITY_ADDRESS_INFORMATION;
                    switch (gap_random_address_get_mode()){
                        case GAP_RANDOM_ADDRESS_TYPE_OFF:
                        case GAP_RANDOM_ADDRESS_TYPE_STATIC:
                            // public or static random
                            gap_le_get_own_address(&buffer[1], local_address);
                            break;
                        case GAP_RANDOM_ADDRESS_NON_RESOLVABLE:
                        case GAP_RANDOM_ADDRESS_RESOLVABLE:
                            // fallback to public
                            gap_local_bd_addr(local_address);
                            buffer[1] = 0;
                            break;
                    }
                    reverse_bd_addr(local_address, &buffer[2]);
                    l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                    sm_timeout_reset(connection);
                    return;
                }
                if (setup->sm_key_distribution_send_set &   SM_KEYDIST_FLAG_SIGNING_IDENTIFICATION){
                    setup->sm_key_distribution_send_set &= ~SM_KEYDIST_FLAG_SIGNING_IDENTIFICATION;

                    // hack to reproduce test runs
                    if (test_use_fixed_local_csrk){
                        memset(setup->sm_local_csrk, 0xcc, 16);
                    }

                    uint8_t buffer[17];
                    buffer[0] = SM_CODE_SIGNING_INFORMATION;
                    reverse_128(setup->sm_local_csrk, &buffer[1]);
                    l2cap_send_connectionless(connection->sm_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                    sm_timeout_reset(connection);
                    return;
                }

                // keys are sent
                if (IS_RESPONDER(connection->sm_role)){
                    // slave -> receive master keys if any
                    if (sm_key_distribution_all_received(connection)){
                        sm_key_distribution_handle_all_received(connection);
                        connection->sm_engine_state = SM_RESPONDER_IDLE;
                        sm_notify_client_status_reason(connection, ERROR_CODE_SUCCESS, 0);
                        sm_done_for_handle(connection->sm_handle);
                    } else {
                        connection->sm_engine_state = SM_PH3_RECEIVE_KEYS;
                    }
                } else {
                    // master -> all done
                    connection->sm_engine_state = SM_INITIATOR_CONNECTED;
                    sm_notify_client_status_reason(connection, ERROR_CODE_SUCCESS, 0);
                    sm_done_for_handle(connection->sm_handle);
                }
                break;

            default:
                break;
        }

        // check again if active connection was released
        if (sm_active_connection_handle != HCI_CON_HANDLE_INVALID) break;
    }
}

// sm_aes128_state stays active
static void sm_handle_encryption_result_enc_a(void *arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    sm_c1_t3(sm_aes128_ciphertext, setup->sm_m_address, setup->sm_s_address, setup->sm_c1_t3_value);
    btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, setup->sm_tk, setup->sm_c1_t3_value, setup->sm_local_confirm, sm_handle_encryption_result_enc_b, connection);
}

static void sm_handle_encryption_result_enc_b(void *arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    sm_aes128_state = SM_AES128_IDLE;
    log_info_key("c1!", setup->sm_local_confirm);
    connection->sm_engine_state = SM_PH2_C1_SEND_PAIRING_CONFIRM;
    sm_run();
}

// sm_aes128_state stays active
static void sm_handle_encryption_result_enc_c(void *arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    sm_c1_t3(sm_aes128_ciphertext, setup->sm_m_address, setup->sm_s_address, setup->sm_c1_t3_value);
    btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, setup->sm_tk, setup->sm_c1_t3_value, sm_aes128_ciphertext, sm_handle_encryption_result_enc_d, connection);
}

static void sm_handle_encryption_result_enc_d(void * arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    sm_aes128_state = SM_AES128_IDLE;
    log_info_key("c1!", sm_aes128_ciphertext);
    if (memcmp(setup->sm_peer_confirm, sm_aes128_ciphertext, 16) != 0){
        setup->sm_pairing_failed_reason = SM_REASON_CONFIRM_VALUE_FAILED;
        connection->sm_engine_state = SM_GENERAL_SEND_PAIRING_FAILED;
        sm_run();
        return;
    }
    if (IS_RESPONDER(connection->sm_role)){
        connection->sm_engine_state = SM_PH2_SEND_PAIRING_RANDOM;
        sm_run();
    } else {
        sm_s1_r_prime(setup->sm_peer_random, setup->sm_local_random, sm_aes128_plaintext);
        sm_aes128_state = SM_AES128_ACTIVE;
        btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, setup->sm_tk, sm_aes128_plaintext, setup->sm_ltk, sm_handle_encryption_result_enc_stk, connection);
    }
}

static void sm_handle_encryption_result_enc_stk(void *arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    sm_aes128_state = SM_AES128_IDLE;
    sm_truncate_key(setup->sm_ltk, connection->sm_actual_encryption_key_size);
    log_info_key("stk", setup->sm_ltk);
    if (IS_RESPONDER(connection->sm_role)){
        connection->sm_engine_state = SM_RESPONDER_PH2_SEND_LTK_REPLY;
    } else {
        connection->sm_engine_state = SM_INITIATOR_PH3_SEND_START_ENCRYPTION;
    }
    sm_run();
}

// sm_aes128_state stays active
static void sm_handle_encryption_result_enc_ph3_y(void *arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    setup->sm_local_y = big_endian_read_16(sm_aes128_ciphertext, 14);
    log_info_hex16("y", setup->sm_local_y);
    // PH3B3 - calculate EDIV
    setup->sm_local_ediv = setup->sm_local_y ^ setup->sm_local_div;
    log_info_hex16("ediv", setup->sm_local_ediv);
    // PH3B4 - calculate LTK         - enc
    // LTK = d1(ER, DIV, 0))
    sm_d1_d_prime(setup->sm_local_div, 0, sm_aes128_plaintext);
    btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, sm_persistent_er, sm_aes128_plaintext, setup->sm_ltk, sm_handle_encryption_result_enc_ph3_ltk, connection);
}

#ifdef ENABLE_LE_PERIPHERAL
// sm_aes128_state stays active
static void sm_handle_encryption_result_enc_ph4_y(void *arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    setup->sm_local_y = big_endian_read_16(sm_aes128_ciphertext, 14);
    log_info_hex16("y", setup->sm_local_y);

    // PH3B3 - calculate DIV
    setup->sm_local_div = setup->sm_local_y ^ setup->sm_local_ediv;
    log_info_hex16("ediv", setup->sm_local_ediv);
    // PH3B4 - calculate LTK         - enc
    // LTK = d1(ER, DIV, 0))
    sm_d1_d_prime(setup->sm_local_div, 0, sm_aes128_plaintext);
    btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, sm_persistent_er, sm_aes128_plaintext, setup->sm_ltk, sm_handle_encryption_result_enc_ph4_ltk, connection);
}
#endif

// sm_aes128_state stays active
static void sm_handle_encryption_result_enc_ph3_ltk(void *arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    log_info_key("ltk", setup->sm_ltk);
    // calc CSRK next
    sm_d1_d_prime(setup->sm_local_div, 1, sm_aes128_plaintext);
    btstack_crypto_aes128_encrypt(&sm_crypto_aes128_request, sm_persistent_er, sm_aes128_plaintext, setup->sm_local_csrk, sm_handle_encryption_result_enc_csrk, connection);
}

static void sm_handle_encryption_result_enc_csrk(void *arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    sm_aes128_state = SM_AES128_IDLE;
    log_info_key("csrk", setup->sm_local_csrk);
    if (setup->sm_key_distribution_send_set){
        connection->sm_engine_state = SM_PH3_DISTRIBUTE_KEYS;
    } else {
        // no keys to send, just continue
        if (IS_RESPONDER(connection->sm_role)){
            // slave -> receive master keys
            connection->sm_engine_state = SM_PH3_RECEIVE_KEYS;
        } else {
            if (setup->sm_use_secure_connections && (setup->sm_key_distribution_received_set & SM_KEYDIST_FLAG_IDENTITY_ADDRESS_INFORMATION)){
                connection->sm_engine_state = SM_SC_W2_CALCULATE_H6_ILK;
            } else {
                // master -> all done
                connection->sm_engine_state = SM_INITIATOR_CONNECTED;
                sm_done_for_handle(connection->sm_handle);
            }
        }
    }
    sm_run();
}

#ifdef ENABLE_LE_PERIPHERAL
static void sm_handle_encryption_result_enc_ph4_ltk(void *arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    sm_aes128_state = SM_AES128_IDLE;
    sm_truncate_key(setup->sm_ltk, connection->sm_actual_encryption_key_size);
    log_info_key("ltk", setup->sm_ltk);
    connection->sm_engine_state = SM_RESPONDER_PH4_SEND_LTK_REPLY;
    sm_run();
}
#endif

static void sm_handle_encryption_result_address_resolution(void *arg){
    UNUSED(arg);
    sm_aes128_state = SM_AES128_IDLE;
    sm_address_resolution_ah_calculation_active = 0;
    // compare calulated address against connecting device
    uint8_t * hash = &sm_aes128_ciphertext[13];
    if (memcmp(&sm_address_resolution_address[3], hash, 3) == 0){
        log_info("LE Device Lookup: matched resolvable private address");
        sm_address_resolution_handle_event(ADDRESS_RESOLUTION_SUCEEDED);
        sm_run();
        return;
    }
    // no match, try next
    sm_address_resolution_test++;
    sm_run();
}

static void sm_handle_encryption_result_dkg_irk(void *arg){
    UNUSED(arg);
    sm_aes128_state = SM_AES128_IDLE;
    log_info_key("irk", sm_persistent_irk);
    dkg_state = DKG_CALC_DHK;
    sm_run();
}

static void sm_handle_encryption_result_dkg_dhk(void *arg){
    UNUSED(arg);
    sm_aes128_state = SM_AES128_IDLE;
    log_info_key("dhk", sm_persistent_dhk);
    dkg_state = DKG_READY;
    // DKG calculation complete => SM Init Finished
    sm_run();
}

static void sm_handle_encryption_result_rau(void *arg){
    UNUSED(arg);
    sm_aes128_state = SM_AES128_IDLE;
    memcpy(&sm_random_address[3], &sm_aes128_ciphertext[13], 3);
    rau_state = RAU_SET_ADDRESS;
    sm_run();
}

static void sm_handle_random_result_rau(void * arg){
    UNUSED(arg);
    // non-resolvable vs. resolvable
    switch (gap_random_adress_type){
        case GAP_RANDOM_ADDRESS_RESOLVABLE:
            // resolvable: use random as prand and calc address hash
            // "The two most significant bits of prand shall be equal to ‘0’ and ‘1"
            sm_random_address[0] &= 0x3f;
            sm_random_address[0] |= 0x40;
            rau_state = RAU_GET_ENC;
            break;
        case GAP_RANDOM_ADDRESS_NON_RESOLVABLE:
        default:
            // "The two most significant bits of the address shall be equal to ‘0’""
            sm_random_address[0] &= 0x3f;
            rau_state = RAU_SET_ADDRESS;
            break;
    }
    sm_run();
}

#ifdef ENABLE_LE_SECURE_CONNECTIONS
static void sm_handle_random_result_sc_get_random(void * arg){
    sm_connection_t * connection = (sm_connection_t*) arg;

    // OOB
    if (setup->sm_stk_generation_method == OOB){
        connection->sm_engine_state = SM_SC_SEND_PAIRING_RANDOM;
        sm_run();
        return;
    } 

    // initiator & jw/nc -> send pairing random
    if (connection->sm_role == 0 && sm_just_works_or_numeric_comparison(setup->sm_stk_generation_method)){
        connection->sm_engine_state = SM_SC_SEND_PAIRING_RANDOM;
    } else {
        connection->sm_engine_state = SM_SC_W2_CMAC_FOR_CONFIRMATION;
    }
    sm_run();
}
#endif

static void sm_handle_random_result_ph2_random(void * arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    connection->sm_engine_state = SM_PH2_C1_GET_ENC_A;
    sm_run();
}

static void sm_handle_random_result_ph2_tk(void * arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    sm_reset_tk();
    uint32_t tk;
    if (sm_fixed_passkey_in_display_role == 0xffffffff){
        // map random to 0-999999 without speding much cycles on a modulus operation
        tk = little_endian_read_32(sm_random_data,0);
        tk = tk & 0xfffff;  // 1048575
        if (tk >= 999999){
            tk = tk - 999999;
        }
    } else {
        // override with pre-defined passkey
        tk = sm_fixed_passkey_in_display_role;
    }
    big_endian_store_32(setup->sm_tk, 12, tk);
    if (IS_RESPONDER(connection->sm_role)){
        connection->sm_engine_state = SM_RESPONDER_PH1_SEND_PAIRING_RESPONSE;
    } else {
        if (setup->sm_use_secure_connections){
            connection->sm_engine_state = SM_SC_SEND_PUBLIC_KEY_COMMAND;
        } else {
            connection->sm_engine_state = SM_PH1_W4_USER_RESPONSE;
            sm_trigger_user_response(connection);
            // response_idle == nothing <--> sm_trigger_user_response() did not require response
            if (setup->sm_user_response == SM_USER_RESPONSE_IDLE){
                btstack_crypto_random_generate(&sm_crypto_random_request, setup->sm_local_random, 16, &sm_handle_random_result_ph2_random, connection);
            }
        }
    }   
    sm_run(); 
}

static void sm_handle_random_result_ph3_div(void * arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    // use 16 bit from random value as div
    setup->sm_local_div = big_endian_read_16(sm_random_data, 0);
    log_info_hex16("div", setup->sm_local_div);
    connection->sm_engine_state = SM_PH3_Y_GET_ENC;
    sm_run();
}

static void sm_handle_random_result_ph3_random(void * arg){
    sm_connection_t * connection = (sm_connection_t*) arg;
    reverse_64(sm_random_data, setup->sm_local_rand);
    // no db for encryption size hack: encryption size is stored in lowest nibble of setup->sm_local_rand
    setup->sm_local_rand[7] = (setup->sm_local_rand[7] & 0xf0) + (connection->sm_actual_encryption_key_size - 1);
    // no db for authenticated flag hack: store flag in bit 4 of LSB
    setup->sm_local_rand[7] = (setup->sm_local_rand[7] & 0xef) + (connection->sm_connection_authenticated << 4);
    btstack_crypto_random_generate(&sm_crypto_random_request, sm_random_data, 2, &sm_handle_random_result_ph3_div, connection);
}

static void sm_event_packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){

    UNUSED(channel);    // ok: there is no channel
    UNUSED(size);       // ok: fixed format HCI events

    sm_connection_t  * sm_conn;
    hci_con_handle_t con_handle;

    switch (packet_type) {

		case HCI_EVENT_PACKET:
			switch (hci_event_packet_get_type(packet)) {

                case BTSTACK_EVENT_STATE:
					// bt stack activated, get started
					if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING){
                        log_info("HCI Working!");

                        dkg_state = sm_persistent_irk_ready ? DKG_CALC_DHK : DKG_CALC_IRK;

                        // trigger Random Address generation if requested before
                        switch (gap_random_adress_type){
                            case GAP_RANDOM_ADDRESS_TYPE_OFF:
                                rau_state = RAU_IDLE;
                                break;
                            case GAP_RANDOM_ADDRESS_TYPE_STATIC:
                                rau_state = RAU_SET_ADDRESS;
                                break;
                            default:
                                rau_state = RAU_W4_RANDOM;
                                btstack_crypto_random_generate(&sm_crypto_random_request, sm_random_address, 8, &sm_handle_random_result_rau, NULL);
                                break;
                        }
                        sm_run();
					}
					break;

                case HCI_EVENT_LE_META:
                    switch (packet[2]) {
                        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:

                            log_info("sm: connected");

                            if (packet[3]) return; // connection failed

                            con_handle = little_endian_read_16(packet, 4);
                            sm_conn = sm_get_connection_for_handle(con_handle);
                            if (!sm_conn) break;

                            sm_conn->sm_handle = con_handle;
                            sm_conn->sm_role = packet[6];
                            sm_conn->sm_peer_addr_type = packet[7];
                            reverse_bd_addr(&packet[8], sm_conn->sm_peer_address);

                            log_info("New sm_conn, role %s", sm_conn->sm_role ? "slave" : "master");

                            // reset security properties
                            sm_conn->sm_connection_encrypted = 0;
                            sm_conn->sm_connection_authenticated = 0;
                            sm_conn->sm_connection_authorization_state = AUTHORIZATION_UNKNOWN;
                            sm_conn->sm_le_db_index = -1;

                            // prepare CSRK lookup (does not involve setup)
                            sm_conn->sm_irk_lookup_state = IRK_LOOKUP_W4_READY;

                            // just connected -> everything else happens in sm_run()
                            if (IS_RESPONDER(sm_conn->sm_role)){
                                // slave - state already could be SM_RESPONDER_SEND_SECURITY_REQUEST instead
                                if (sm_conn->sm_engine_state == SM_GENERAL_IDLE){
                                    if (sm_slave_request_security) {
                                        // request security if requested by app
                                        sm_conn->sm_engine_state = SM_RESPONDER_SEND_SECURITY_REQUEST;
                                    } else {
                                        // otherwise, wait for pairing request
                                        sm_conn->sm_engine_state = SM_RESPONDER_IDLE;
                                    }
                                }
                                break;
                            } else {
                                // master
                                sm_conn->sm_engine_state = SM_INITIATOR_CONNECTED;
                            }
                            break;

                        case HCI_SUBEVENT_LE_LONG_TERM_KEY_REQUEST:
                            con_handle = little_endian_read_16(packet, 3);
                            sm_conn = sm_get_connection_for_handle(con_handle);
                            if (!sm_conn) break;

                            log_info("LTK Request: state %u", sm_conn->sm_engine_state);
                            if (sm_conn->sm_engine_state == SM_RESPONDER_PH2_W4_LTK_REQUEST){
                                sm_conn->sm_engine_state = SM_PH2_CALC_STK;
                                break;
                            }
                            if (sm_conn->sm_engine_state == SM_SC_W4_LTK_REQUEST_SC){
                                // PH2 SEND LTK as we need to exchange keys in PH3
                                sm_conn->sm_engine_state = SM_RESPONDER_PH2_SEND_LTK_REPLY;
                                break;
                            }

                            // store rand and ediv
                            reverse_64(&packet[5], sm_conn->sm_local_rand);
                            sm_conn->sm_local_ediv = little_endian_read_16(packet, 13);

                            // For Legacy Pairing (<=> EDIV != 0 || RAND != NULL), we need to recalculated our LTK as a
                            // potentially stored LTK is from the master
                            if (sm_conn->sm_local_ediv != 0 || !sm_is_null_random(sm_conn->sm_local_rand)){
                                if (sm_reconstruct_ltk_without_le_device_db_entry){
                                    sm_conn->sm_engine_state = SM_RESPONDER_PH0_RECEIVED_LTK_REQUEST;
                                    break;
                                }
                                // additionally check if remote is in LE Device DB if requested
                                switch(sm_conn->sm_irk_lookup_state){
                                    case IRK_LOOKUP_FAILED:
                                        log_info("LTK Request: device not in device db");
                                        sm_conn->sm_engine_state = SM_RESPONDER_PH0_SEND_LTK_REQUESTED_NEGATIVE_REPLY;
                                        break;
                                    case IRK_LOOKUP_SUCCEEDED:
                                        sm_conn->sm_engine_state = SM_RESPONDER_PH0_RECEIVED_LTK_REQUEST;
                                        break;
                                    default:
                                        // wait for irk look doen
                                        sm_conn->sm_engine_state = SM_RESPONDER_PH0_RECEIVED_LTK_W4_IRK;
                                        break;
                                }
                                break;
                            }

#ifdef ENABLE_LE_SECURE_CONNECTIONS
                            sm_conn->sm_engine_state = SM_SC_RECEIVED_LTK_REQUEST;
#else
                            log_info("LTK Request: ediv & random are empty, but LE Secure Connections not supported");
                            sm_conn->sm_engine_state = SM_RESPONDER_PH0_SEND_LTK_REQUESTED_NEGATIVE_REPLY;
#endif
                            break;

                        default:
                            break;
                    }
                    break;

                case HCI_EVENT_ENCRYPTION_CHANGE:
                    con_handle = little_endian_read_16(packet, 3);
                    sm_conn = sm_get_connection_for_handle(con_handle);
                    if (!sm_conn) break;

                    sm_conn->sm_connection_encrypted = packet[5];
                    log_info("Encryption state change: %u, key size %u", sm_conn->sm_connection_encrypted,
                        sm_conn->sm_actual_encryption_key_size);
                    log_info("event handler, state %u", sm_conn->sm_engine_state);
                    if (!sm_conn->sm_connection_encrypted) break;
                    // continue if part of initial pairing
                    switch (sm_conn->sm_engine_state){
                        case SM_INITIATOR_PH0_W4_CONNECTION_ENCRYPTED:
                            sm_conn->sm_engine_state = SM_INITIATOR_CONNECTED;
                            sm_done_for_handle(sm_conn->sm_handle);
                            break;
                        case SM_PH2_W4_CONNECTION_ENCRYPTED:
                            if (IS_RESPONDER(sm_conn->sm_role)){
                                // slave
                                if (setup->sm_use_secure_connections){
                                    sm_conn->sm_engine_state = SM_PH3_DISTRIBUTE_KEYS;
                                } else {
                                    btstack_crypto_random_generate(&sm_crypto_random_request, sm_random_data, 8, &sm_handle_random_result_ph3_random, sm_conn);
                                }
                            } else {
                                // master
                                if (sm_key_distribution_all_received(sm_conn)){
                                    // skip receiving keys as there are none
                                    sm_key_distribution_handle_all_received(sm_conn);
                                    btstack_crypto_random_generate(&sm_crypto_random_request, sm_random_data, 8, &sm_handle_random_result_ph3_random, sm_conn);
                                } else {
                                    sm_conn->sm_engine_state = SM_PH3_RECEIVE_KEYS;
                                }
                            }
                            break;
                        default:
                            break;
                    }
                    break;

                case HCI_EVENT_ENCRYPTION_KEY_REFRESH_COMPLETE:
                    con_handle = little_endian_read_16(packet, 3);
                    sm_conn = sm_get_connection_for_handle(con_handle);
                    if (!sm_conn) break;

                    log_info("Encryption key refresh complete, key size %u", sm_conn->sm_actual_encryption_key_size);
                    log_info("event handler, state %u", sm_conn->sm_engine_state);
                    // continue if part of initial pairing
                    switch (sm_conn->sm_engine_state){
                        case SM_INITIATOR_PH0_W4_CONNECTION_ENCRYPTED:
                            sm_conn->sm_engine_state = SM_INITIATOR_CONNECTED;
                            sm_done_for_handle(sm_conn->sm_handle);
                            break;
                        case SM_PH2_W4_CONNECTION_ENCRYPTED:
                            if (IS_RESPONDER(sm_conn->sm_role)){
                                // slave
                                btstack_crypto_random_generate(&sm_crypto_random_request, sm_random_data, 8, &sm_handle_random_result_ph3_random, sm_conn);
                            } else {
                                // master
                                sm_conn->sm_engine_state = SM_PH3_RECEIVE_KEYS;
                            }
                            break;
                        default:
                            break;
                    }
                    break;


                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    con_handle = little_endian_read_16(packet, 3);
                    sm_done_for_handle(con_handle);
                    sm_conn = sm_get_connection_for_handle(con_handle);
                    if (!sm_conn) break;

                    // delete stored bonding on disconnect with authentication failure in ph0
                    if (sm_conn->sm_role == 0
                        && sm_conn->sm_engine_state == SM_INITIATOR_PH0_W4_CONNECTION_ENCRYPTED
                        && packet[2] == ERROR_CODE_AUTHENTICATION_FAILURE){
                        le_device_db_remove(sm_conn->sm_le_db_index);
                    }

                    // pairing failed, if it was ongoing
                    if (sm_conn->sm_engine_state != SM_INITIATOR_CONNECTED && sm_conn->sm_engine_state != SM_GENERAL_IDLE){
                        sm_notify_client_status_reason(sm_conn, ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION, 0);
                    }

                    sm_conn->sm_engine_state = SM_GENERAL_IDLE;
                    sm_conn->sm_handle = 0;
                    break;

				case HCI_EVENT_COMMAND_COMPLETE:
                    if (HCI_EVENT_IS_COMMAND_COMPLETE(packet, hci_read_bd_addr)){
                        // set local addr for le device db
                        bd_addr_t addr;
                        reverse_bd_addr(&packet[OFFSET_OF_DATA_IN_COMMAND_COMPLETE + 1], addr);
                        le_device_db_set_local_bd_addr(addr);
                    }
                    break;
                default:
                    break;
			}
            break;
        default:
            break;
	}

    sm_run();
}

static inline int sm_calc_actual_encryption_key_size(int other){
    if (other < sm_min_encryption_key_size) return 0;
    if (other < sm_max_encryption_key_size) return other;
    return sm_max_encryption_key_size;
}


#ifdef ENABLE_LE_SECURE_CONNECTIONS
static int sm_just_works_or_numeric_comparison(stk_generation_method_t method){
    switch (method){
        case JUST_WORKS:
        case NUMERIC_COMPARISON:
            return 1;
        default:
            return 0;
    }
}
// responder

static int sm_passkey_used(stk_generation_method_t method){
    switch (method){
        case PK_RESP_INPUT:
            return 1;
        default:
            return 0;
    }
}

static int sm_passkey_entry(stk_generation_method_t method){
    switch (method){
        case PK_RESP_INPUT:
        case PK_INIT_INPUT:
        case PK_BOTH_INPUT:
            return 1;
        default:
            return 0;
    }
}

#endif

/**
 * @return ok
 */
static int sm_validate_stk_generation_method(void){
    // check if STK generation method is acceptable by client
    switch (setup->sm_stk_generation_method){
        case JUST_WORKS:
            return (sm_accepted_stk_generation_methods & SM_STK_GENERATION_METHOD_JUST_WORKS) != 0;
        case PK_RESP_INPUT:
        case PK_INIT_INPUT:
        case PK_BOTH_INPUT:
            return (sm_accepted_stk_generation_methods & SM_STK_GENERATION_METHOD_PASSKEY) != 0;
        case OOB:
            return (sm_accepted_stk_generation_methods & SM_STK_GENERATION_METHOD_OOB) != 0;
        case NUMERIC_COMPARISON:
            return (sm_accepted_stk_generation_methods & SM_STK_GENERATION_METHOD_NUMERIC_COMPARISON) != 0;
            return 1;
        default:
            return 0;
    }
}

// size of complete sm_pdu used to validate input
static const uint8_t sm_pdu_size[] = {
    0,  // 0x00 invalid opcode
    7,  // 0x01 pairing request
    7,  // 0x02 pairing response
    17, // 0x03 pairing confirm
    17, // 0x04 pairing random
    2,  // 0x05 pairing failed
    17, // 0x06 encryption information
    11, // 0x07 master identification
    17, // 0x08 identification information
    8,  // 0x09 identify address information
    17, // 0x0a signing information
    2,  // 0x0b security request
    65, // 0x0c pairing public key
    17, // 0x0d pairing dhk check
    2,  // 0x0e keypress notification
};

static void sm_pdu_handler(uint8_t packet_type, hci_con_handle_t con_handle, uint8_t *packet, uint16_t size){

    if (packet_type == HCI_EVENT_PACKET && packet[0] == L2CAP_EVENT_CAN_SEND_NOW){
        sm_run();
    }

    if (packet_type != SM_DATA_PACKET) return;
    if (size == 0) return;

    uint8_t sm_pdu_code = packet[0];

    // validate pdu size
    if (sm_pdu_code >= sizeof(sm_pdu_size)) return;
    if (sm_pdu_size[sm_pdu_code] != size)   return;

    sm_connection_t * sm_conn = sm_get_connection_for_handle(con_handle);
    if (!sm_conn) return;

    if (sm_pdu_code == SM_CODE_PAIRING_FAILED){
        sm_notify_client_status_reason(sm_conn, ERROR_CODE_AUTHENTICATION_FAILURE, packet[1]);
        sm_done_for_handle(con_handle);
        sm_conn->sm_engine_state = sm_conn->sm_role ? SM_RESPONDER_IDLE : SM_INITIATOR_CONNECTED;
        return;
    }

    log_debug("sm_pdu_handler: state %u, pdu 0x%02x", sm_conn->sm_engine_state, sm_pdu_code);

    int err;
    UNUSED(err);

    if (sm_pdu_code == SM_CODE_KEYPRESS_NOTIFICATION){
        uint8_t buffer[5];
        buffer[0] = SM_EVENT_KEYPRESS_NOTIFICATION;
        buffer[1] = 3;
        little_endian_store_16(buffer, 2, con_handle);
        buffer[4] = packet[1];
        sm_dispatch_event(HCI_EVENT_PACKET, 0, buffer, sizeof(buffer));
        return;
    }

    switch (sm_conn->sm_engine_state){

        // a sm timeout requries a new physical connection
        case SM_GENERAL_TIMEOUT:
            return;

#ifdef ENABLE_LE_CENTRAL

        // Initiator
        case SM_INITIATOR_CONNECTED:
            if ((sm_pdu_code != SM_CODE_SECURITY_REQUEST) || (sm_conn->sm_role)){
                sm_pdu_received_in_wrong_state(sm_conn);
                break;
            }
            if (sm_conn->sm_irk_lookup_state == IRK_LOOKUP_FAILED){
                sm_conn->sm_engine_state = SM_INITIATOR_PH1_W2_SEND_PAIRING_REQUEST;
                break;
            }
            if (sm_conn->sm_irk_lookup_state == IRK_LOOKUP_SUCCEEDED){
                sm_key_t ltk;
                le_device_db_encryption_get(sm_conn->sm_le_db_index, NULL, NULL, ltk, NULL, NULL, NULL);
                if (!sm_is_null_key(ltk)){
                    log_info("sm: Setting up previous ltk/ediv/rand for device index %u", sm_conn->sm_le_db_index);
                    sm_conn->sm_engine_state = SM_INITIATOR_PH0_HAS_LTK;
                } else {
                    sm_conn->sm_engine_state = SM_INITIATOR_PH1_W2_SEND_PAIRING_REQUEST;
                }
                break;
            }
            // otherwise, store security request
            sm_conn->sm_security_request_received = 1;
            break;

        case SM_INITIATOR_PH1_W4_PAIRING_RESPONSE:
            if (sm_pdu_code != SM_CODE_PAIRING_RESPONSE){
                sm_pdu_received_in_wrong_state(sm_conn);
                break;
            }

            // store pairing request
            memcpy(&setup->sm_s_pres, packet, sizeof(sm_pairing_packet_t));
            err = sm_stk_generation_init(sm_conn);

#ifdef ENABLE_TESTING_SUPPORT
            if (0 < test_pairing_failure && test_pairing_failure < SM_REASON_DHKEY_CHECK_FAILED){
                log_info("testing_support: abort with pairing failure %u", test_pairing_failure);
                err = test_pairing_failure;
            }
#endif

            if (err){
                setup->sm_pairing_failed_reason = err;
                sm_conn->sm_engine_state = SM_GENERAL_SEND_PAIRING_FAILED;
                break;
            }

            // generate random number first, if we need to show passkey
            if (setup->sm_stk_generation_method == PK_RESP_INPUT){
                btstack_crypto_random_generate(&sm_crypto_random_request, sm_random_data, 8, &sm_handle_random_result_ph2_tk, sm_conn);
                break;
            }

#ifdef ENABLE_LE_SECURE_CONNECTIONS
            if (setup->sm_use_secure_connections){
                // SC Numeric Comparison will trigger user response after public keys & nonces have been exchanged
                if (setup->sm_stk_generation_method == JUST_WORKS){
                    sm_conn->sm_engine_state = SM_PH1_W4_USER_RESPONSE;
                    sm_trigger_user_response(sm_conn);
                    if (setup->sm_user_response == SM_USER_RESPONSE_IDLE){
                        sm_conn->sm_engine_state = SM_SC_SEND_PUBLIC_KEY_COMMAND;
                    }
                } else {
                    sm_conn->sm_engine_state = SM_SC_SEND_PUBLIC_KEY_COMMAND;
                }
                break;
            }
#endif
            sm_conn->sm_engine_state = SM_PH1_W4_USER_RESPONSE;
            sm_trigger_user_response(sm_conn);
            // response_idle == nothing <--> sm_trigger_user_response() did not require response
            if (setup->sm_user_response == SM_USER_RESPONSE_IDLE){
                btstack_crypto_random_generate(&sm_crypto_random_request, setup->sm_local_random, 16, &sm_handle_random_result_ph2_random, sm_conn);
            }
            break;

        case SM_INITIATOR_PH2_W4_PAIRING_CONFIRM:
            if (sm_pdu_code != SM_CODE_PAIRING_CONFIRM){
                sm_pdu_received_in_wrong_state(sm_conn);
                break;
            }

            // store s_confirm
            reverse_128(&packet[1], setup->sm_peer_confirm);

#ifdef ENABLE_TESTING_SUPPORT
            if (test_pairing_failure == SM_REASON_CONFIRM_VALUE_FAILED){
                log_info("testing_support: reset confirm value");
                memset(setup->sm_peer_confirm, 0, 16);
            }
#endif
            sm_conn->sm_engine_state = SM_PH2_SEND_PAIRING_RANDOM;
            break;

        case SM_INITIATOR_PH2_W4_PAIRING_RANDOM:
            if (sm_pdu_code != SM_CODE_PAIRING_RANDOM){
                sm_pdu_received_in_wrong_state(sm_conn);
                break;;
            }

            // received random value
            reverse_128(&packet[1], setup->sm_peer_random);
            sm_conn->sm_engine_state = SM_PH2_C1_GET_ENC_C;
            break;
#endif

#ifdef ENABLE_LE_PERIPHERAL
        // Responder
        case SM_RESPONDER_IDLE:
        case SM_RESPONDER_SEND_SECURITY_REQUEST:
        case SM_RESPONDER_PH1_W4_PAIRING_REQUEST:
            if (sm_pdu_code != SM_CODE_PAIRING_REQUEST){
                sm_pdu_received_in_wrong_state(sm_conn);
                break;;
            }

            // store pairing request
            memcpy(&sm_conn->sm_m_preq, packet, sizeof(sm_pairing_packet_t));
            sm_conn->sm_engine_state = SM_RESPONDER_PH1_PAIRING_REQUEST_RECEIVED;
            break;
#endif

#ifdef ENABLE_LE_SECURE_CONNECTIONS
        case SM_SC_W4_PUBLIC_KEY_COMMAND:
            if (sm_pdu_code != SM_CODE_PAIRING_PUBLIC_KEY){
                sm_pdu_received_in_wrong_state(sm_conn);
                break;
            }

            // store public key for DH Key calculation
            reverse_256(&packet[01], &setup->sm_peer_q[0]);
            reverse_256(&packet[33], &setup->sm_peer_q[32]);

            // validate public key
            err = btstack_crypto_ecc_p256_validate_public_key(setup->sm_peer_q);
            if (err){
                log_error("sm: peer public key invalid %x", err);
                // uses "unspecified reason", there is no "public key invalid" error code
                sm_pdu_received_in_wrong_state(sm_conn);
                break;
            }

            // start calculating dhkey
            btstack_crypto_ecc_p256_calculate_dhkey(&sm_crypto_ecc_p256_request, setup->sm_peer_q, setup->sm_dhkey, sm_sc_dhkey_calculated, sm_conn);


            log_info("public key received, generation method %u", setup->sm_stk_generation_method);
            if (IS_RESPONDER(sm_conn->sm_role)){
                // responder
                sm_conn->sm_engine_state = SM_SC_SEND_PUBLIC_KEY_COMMAND;
            } else {
                // initiator
                // stk generation method
                // passkey entry: notify app to show passkey or to request passkey
                switch (setup->sm_stk_generation_method){
                    case JUST_WORKS:
                    case NUMERIC_COMPARISON:
                        sm_conn->sm_engine_state = SM_SC_W4_CONFIRMATION;
                        break;
                    case PK_RESP_INPUT:
                        sm_sc_start_calculating_local_confirm(sm_conn);
                        break;
                    case PK_INIT_INPUT:
                    case PK_BOTH_INPUT:
                        if (setup->sm_user_response != SM_USER_RESPONSE_PASSKEY){
                            sm_conn->sm_engine_state = SM_SC_W4_USER_RESPONSE;
                            break;
                        }
                        sm_sc_start_calculating_local_confirm(sm_conn);
                        break;
                    case OOB:
                        // generate Nx
                        log_info("Generate Na");
                        btstack_crypto_random_generate(&sm_crypto_random_request, setup->sm_local_nonce, 16, &sm_handle_random_result_sc_get_random, sm_conn);
                        break;
                }
            }
            break;

        case SM_SC_W4_CONFIRMATION:
            if (sm_pdu_code != SM_CODE_PAIRING_CONFIRM){
                sm_pdu_received_in_wrong_state(sm_conn);
                break;
            }
            // received confirm value
            reverse_128(&packet[1], setup->sm_peer_confirm);

#ifdef ENABLE_TESTING_SUPPORT
            if (test_pairing_failure == SM_REASON_CONFIRM_VALUE_FAILED){
                log_info("testing_support: reset confirm value");
                memset(setup->sm_peer_confirm, 0, 16);
            }
#endif
            if (IS_RESPONDER(sm_conn->sm_role)){
                // responder
                if (sm_passkey_used(setup->sm_stk_generation_method)){
                    if (setup->sm_user_response != SM_USER_RESPONSE_PASSKEY){
                        // still waiting for passkey
                        sm_conn->sm_engine_state = SM_SC_W4_USER_RESPONSE;
                        break;
                    }
                }
                sm_sc_start_calculating_local_confirm(sm_conn);
            } else {
                // initiator
                if (sm_just_works_or_numeric_comparison(setup->sm_stk_generation_method)){
                    // sm_conn->sm_engine_state = SM_SC_W2_GET_RANDOM_A;
                    btstack_crypto_random_generate(&sm_crypto_random_request, setup->sm_local_nonce, 16, &sm_handle_random_result_sc_get_random, sm_conn);
                } else {
                    sm_conn->sm_engine_state = SM_SC_SEND_PAIRING_RANDOM;
                }
            }
            break;

        case SM_SC_W4_PAIRING_RANDOM:
            if (sm_pdu_code != SM_CODE_PAIRING_RANDOM){
                sm_pdu_received_in_wrong_state(sm_conn);
                break;
            }

            // received random value
            reverse_128(&packet[1], setup->sm_peer_nonce);

            // validate confirm value if Cb = f4(Pkb, Pka, Nb, z)
            // only check for JUST WORK/NC in initiator role OR passkey entry
            if ( (!IS_RESPONDER(sm_conn->sm_role) && sm_just_works_or_numeric_comparison(setup->sm_stk_generation_method)) 
            ||   (sm_passkey_used(setup->sm_stk_generation_method)) ) {
                 sm_conn->sm_engine_state = SM_SC_W2_CMAC_FOR_CHECK_CONFIRMATION;
                 break;
            }

            // OOB
            if (setup->sm_stk_generation_method == OOB){

                // setup local random, set to zero if remote did not receive our data
                log_info("Received nonce, setup local random ra/rb for dhkey check");
                if (IS_RESPONDER(sm_conn->sm_role)){
                    if (sm_pairing_packet_get_oob_data_flag(setup->sm_m_preq) == 0){
                        log_info("Reset rb as A does not have OOB data");
                        memset(setup->sm_rb, 0, 16);
                    } else {
                        memcpy(setup->sm_rb, sm_sc_oob_random, 16);
                        log_info("Use stored rb");
                        log_info_hexdump(setup->sm_rb, 16);
                    }
                }  else {
                    if (sm_pairing_packet_get_oob_data_flag(setup->sm_s_pres) == 0){
                        log_info("Reset ra as B does not have OOB data");
                        memset(setup->sm_ra, 0, 16);
                    } else {
                        memcpy(setup->sm_ra, sm_sc_oob_random, 16);
                        log_info("Use stored ra");
                        log_info_hexdump(setup->sm_ra, 16);
                    }
                }

                // validate confirm value if Cb = f4(PKb, Pkb, rb, 0) for OOB if data received
                if (setup->sm_have_oob_data){
                     sm_conn->sm_engine_state = SM_SC_W2_CMAC_FOR_CHECK_CONFIRMATION;
                     break;
                }
            }

            // TODO: we only get here for Responder role with JW/NC
            sm_sc_state_after_receiving_random(sm_conn);
            break;

        case SM_SC_W2_CALCULATE_G2:
        case SM_SC_W4_CALCULATE_G2:
        case SM_SC_W4_CALCULATE_DHKEY:
        case SM_SC_W2_CALCULATE_F5_SALT:
        case SM_SC_W4_CALCULATE_F5_SALT:
        case SM_SC_W2_CALCULATE_F5_MACKEY:
        case SM_SC_W4_CALCULATE_F5_MACKEY:
        case SM_SC_W2_CALCULATE_F5_LTK:
        case SM_SC_W4_CALCULATE_F5_LTK:
        case SM_SC_W2_CALCULATE_F6_FOR_DHKEY_CHECK:
        case SM_SC_W4_DHKEY_CHECK_COMMAND:
        case SM_SC_W4_CALCULATE_F6_FOR_DHKEY_CHECK:
            if (sm_pdu_code != SM_CODE_PAIRING_DHKEY_CHECK){
                sm_pdu_received_in_wrong_state(sm_conn);
                break;
            }
            // store DHKey Check
            setup->sm_state_vars |= SM_STATE_VAR_DHKEY_COMMAND_RECEIVED;
            reverse_128(&packet[01], setup->sm_peer_dhkey_check);

            // have we been only waiting for dhkey check command?
            if (sm_conn->sm_engine_state == SM_SC_W4_DHKEY_CHECK_COMMAND){
                sm_conn->sm_engine_state = SM_SC_W2_CALCULATE_F6_TO_VERIFY_DHKEY_CHECK;
            }
            break;
#endif

#ifdef ENABLE_LE_PERIPHERAL
        case SM_RESPONDER_PH1_W4_PAIRING_CONFIRM:
            if (sm_pdu_code != SM_CODE_PAIRING_CONFIRM){
                sm_pdu_received_in_wrong_state(sm_conn);
                break;
            }

            // received confirm value
            reverse_128(&packet[1], setup->sm_peer_confirm);

#ifdef ENABLE_TESTING_SUPPORT
            if (test_pairing_failure == SM_REASON_CONFIRM_VALUE_FAILED){
                log_info("testing_support: reset confirm value");
                memset(setup->sm_peer_confirm, 0, 16);
            }
#endif
            // notify client to hide shown passkey
            if (setup->sm_stk_generation_method == PK_INIT_INPUT){
                sm_notify_client_base(SM_EVENT_PASSKEY_DISPLAY_CANCEL, sm_conn->sm_handle, sm_conn->sm_peer_addr_type, sm_conn->sm_peer_address);
            }

            // handle user cancel pairing?
            if (setup->sm_user_response == SM_USER_RESPONSE_DECLINE){
                setup->sm_pairing_failed_reason = SM_REASON_PASSKEY_ENTRY_FAILED;
                sm_conn->sm_engine_state = SM_GENERAL_SEND_PAIRING_FAILED;
                break;
            }

            // wait for user action?
            if (setup->sm_user_response == SM_USER_RESPONSE_PENDING){
                sm_conn->sm_engine_state = SM_PH1_W4_USER_RESPONSE;
                break;
            }

            // calculate and send local_confirm
            btstack_crypto_random_generate(&sm_crypto_random_request, setup->sm_local_random, 16, &sm_handle_random_result_ph2_random, sm_conn);
            break;

        case SM_RESPONDER_PH2_W4_PAIRING_RANDOM:
            if (sm_pdu_code != SM_CODE_PAIRING_RANDOM){
                sm_pdu_received_in_wrong_state(sm_conn);
                break;;
            }

            // received random value
            reverse_128(&packet[1], setup->sm_peer_random);
            sm_conn->sm_engine_state = SM_PH2_C1_GET_ENC_C;
            break;
#endif

        case SM_PH3_RECEIVE_KEYS:
            switch(sm_pdu_code){
                case SM_CODE_ENCRYPTION_INFORMATION:
                    setup->sm_key_distribution_received_set |= SM_KEYDIST_FLAG_ENCRYPTION_INFORMATION;
                    reverse_128(&packet[1], setup->sm_peer_ltk);
                    break;

                case SM_CODE_MASTER_IDENTIFICATION:
                    setup->sm_key_distribution_received_set |= SM_KEYDIST_FLAG_MASTER_IDENTIFICATION;
                    setup->sm_peer_ediv = little_endian_read_16(packet, 1);
                    reverse_64(&packet[3], setup->sm_peer_rand);
                    break;

                case SM_CODE_IDENTITY_INFORMATION:
                    setup->sm_key_distribution_received_set |= SM_KEYDIST_FLAG_IDENTITY_INFORMATION;
                    reverse_128(&packet[1], setup->sm_peer_irk);
                    break;

                case SM_CODE_IDENTITY_ADDRESS_INFORMATION:
                    setup->sm_key_distribution_received_set |= SM_KEYDIST_FLAG_IDENTITY_ADDRESS_INFORMATION;
                    setup->sm_peer_addr_type = packet[1];
                    reverse_bd_addr(&packet[2], setup->sm_peer_address);
                    break;

                case SM_CODE_SIGNING_INFORMATION:
                    setup->sm_key_distribution_received_set |= SM_KEYDIST_FLAG_SIGNING_IDENTIFICATION;
                    reverse_128(&packet[1], setup->sm_peer_csrk);
                    break;
                default:
                    // Unexpected PDU
                    log_info("Unexpected PDU %u in SM_PH3_RECEIVE_KEYS", packet[0]);
                    break;
            }
            // done with key distribution?
            if (sm_key_distribution_all_received(sm_conn)){

                sm_key_distribution_handle_all_received(sm_conn);

                if (IS_RESPONDER(sm_conn->sm_role)){
                    if (setup->sm_use_secure_connections && (setup->sm_key_distribution_received_set & SM_KEYDIST_FLAG_IDENTITY_ADDRESS_INFORMATION)){
                        sm_conn->sm_engine_state = SM_SC_W2_CALCULATE_H6_ILK;
                    } else {
                        sm_conn->sm_engine_state = SM_RESPONDER_IDLE;
                        sm_notify_client_status_reason(sm_conn, ERROR_CODE_SUCCESS, 0);
                        sm_done_for_handle(sm_conn->sm_handle);
                    }
                } else {
                    if (setup->sm_use_secure_connections){
                        sm_conn->sm_engine_state = SM_PH3_DISTRIBUTE_KEYS;
                    } else {
                        btstack_crypto_random_generate(&sm_crypto_random_request, sm_random_data, 8, &sm_handle_random_result_ph3_random, sm_conn);
                    }
                }
            }
            break;
        default:
            // Unexpected PDU
            log_info("Unexpected PDU %u in state %u", packet[0], sm_conn->sm_engine_state);
            break;
    }

    // try to send preparared packet
    sm_run();
}

// Security Manager Client API
void sm_register_oob_data_callback( int (*get_oob_data_callback)(uint8_t address_type, bd_addr_t addr, uint8_t * oob_data)){
    sm_get_oob_data = get_oob_data_callback;
}

void sm_register_sc_oob_data_callback( int (*get_sc_oob_data_callback)(uint8_t address_type, bd_addr_t addr, uint8_t * oob_sc_peer_confirm, uint8_t * oob_sc_peer_random)){
    sm_get_sc_oob_data = get_sc_oob_data_callback;
}

void sm_add_event_handler(btstack_packet_callback_registration_t * callback_handler){
    btstack_linked_list_add_tail(&sm_event_handlers, (btstack_linked_item_t*) callback_handler);
}

void sm_set_accepted_stk_generation_methods(uint8_t accepted_stk_generation_methods){
    sm_accepted_stk_generation_methods = accepted_stk_generation_methods;
}

void sm_set_encryption_key_size_range(uint8_t min_size, uint8_t max_size){
	sm_min_encryption_key_size = min_size;
	sm_max_encryption_key_size = max_size;
}

void sm_set_authentication_requirements(uint8_t auth_req){
#ifndef ENABLE_LE_SECURE_CONNECTIONS
    if (auth_req & SM_AUTHREQ_SECURE_CONNECTION){
        log_error("ENABLE_LE_SECURE_CONNECTIONS not defined, but requested by app. Dropping SC flag");
        auth_req &= ~SM_AUTHREQ_SECURE_CONNECTION;
    }
#endif
    sm_auth_req = auth_req;
}

void sm_set_io_capabilities(io_capability_t io_capability){
    sm_io_capabilities = io_capability;
}

#ifdef ENABLE_LE_PERIPHERAL
void sm_set_request_security(int enable){
    sm_slave_request_security = enable;
}
#endif

void sm_set_er(sm_key_t er){
    memcpy(sm_persistent_er, er, 16);
}

void sm_set_ir(sm_key_t ir){
    memcpy(sm_persistent_ir, ir, 16);
}

// Testing support only
void sm_test_set_irk(sm_key_t irk){
    memcpy(sm_persistent_irk, irk, 16);
    sm_persistent_irk_ready = 1;
}

void sm_test_use_fixed_local_csrk(void){
    test_use_fixed_local_csrk = 1;
}

#ifdef ENABLE_LE_SECURE_CONNECTIONS
static void sm_ec_generated(void * arg){
    UNUSED(arg);
    ec_key_generation_state = EC_KEY_GENERATION_DONE;
}
#endif

#ifdef ENABLE_TESTING_SUPPORT
void sm_test_set_pairing_failure(int reason){
    test_pairing_failure = reason;
}
#endif

void sm_init(void){
    // set some (BTstack default) ER and IR
    int i;
    sm_key_t er;
    sm_key_t ir;
    for (i=0;i<16;i++){
        er[i] = 0x30 + i;
        ir[i] = 0x90 + i;
    }
    sm_set_er(er);
    sm_set_ir(ir);
    // defaults
    sm_accepted_stk_generation_methods = SM_STK_GENERATION_METHOD_JUST_WORKS
                                       | SM_STK_GENERATION_METHOD_OOB
                                       | SM_STK_GENERATION_METHOD_PASSKEY
                                       | SM_STK_GENERATION_METHOD_NUMERIC_COMPARISON;

    sm_max_encryption_key_size = 16;
    sm_min_encryption_key_size = 7;

    sm_fixed_passkey_in_display_role = 0xffffffff;
    sm_reconstruct_ltk_without_le_device_db_entry = 1;

#ifdef USE_CMAC_ENGINE
    sm_cmac_active  = 0;
#endif
    dkg_state = DKG_W4_WORKING;
    rau_state = RAU_W4_WORKING;
    sm_aes128_state = SM_AES128_IDLE;
    sm_address_resolution_test = -1;    // no private address to resolve yet
    sm_address_resolution_ah_calculation_active = 0;
    sm_address_resolution_mode = ADDRESS_RESOLUTION_IDLE;
    sm_address_resolution_general_queue = NULL;

    gap_random_adress_update_period = 15 * 60 * 1000L;
    sm_active_connection_handle = HCI_CON_HANDLE_INVALID;

    test_use_fixed_local_csrk = 0;

    // register for HCI Events from HCI
    hci_event_callback_registration.callback = &sm_event_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // 
    btstack_crypto_init();

    // and L2CAP PDUs + L2CAP_EVENT_CAN_SEND_NOW
    l2cap_register_fixed_channel(sm_pdu_handler, L2CAP_CID_SECURITY_MANAGER_PROTOCOL);

#ifdef ENABLE_LE_SECURE_CONNECTIONS
    ec_key_generation_state = EC_KEY_GENERATION_ACTIVE;
    btstack_crypto_ecc_p256_generate_key(&sm_crypto_ecc_p256_request, ec_q, &sm_ec_generated, NULL);
#endif
}

void sm_use_fixed_passkey_in_display_role(uint32_t passkey){
    sm_fixed_passkey_in_display_role = passkey;
}

void sm_allow_ltk_reconstruction_without_le_device_db_entry(int allow){
    sm_reconstruct_ltk_without_le_device_db_entry = allow;
}

static sm_connection_t * sm_get_connection_for_handle(hci_con_handle_t con_handle){
    hci_connection_t * hci_con = hci_connection_for_handle(con_handle);
    if (!hci_con) return NULL;
    return &hci_con->sm_connection;
}

static void sm_send_security_request_for_connection(sm_connection_t * sm_conn){
    switch (sm_conn->sm_engine_state){
        case SM_GENERAL_IDLE:
        case SM_RESPONDER_IDLE:
            sm_conn->sm_engine_state = SM_RESPONDER_SEND_SECURITY_REQUEST;
            sm_run();
            break;
        default:
            break;
    }
}

/**
 * @brief Trigger Security Request
 */
void sm_send_security_request(hci_con_handle_t con_handle){
    sm_connection_t * sm_conn = sm_get_connection_for_handle(con_handle);
    if (!sm_conn) return;
    sm_send_security_request_for_connection(sm_conn);
}

// request pairing
void sm_request_pairing(hci_con_handle_t con_handle){
    sm_connection_t * sm_conn = sm_get_connection_for_handle(con_handle);
    if (!sm_conn) return;     // wrong connection

    log_info("sm_request_pairing in role %u, state %u", sm_conn->sm_role, sm_conn->sm_engine_state);
    if (IS_RESPONDER(sm_conn->sm_role)){
        sm_send_security_request_for_connection(sm_conn);
    } else {
        // used as a trigger to start central/master/initiator security procedures
        uint16_t ediv;
        sm_key_t ltk;
        if (sm_conn->sm_engine_state == SM_INITIATOR_CONNECTED){
            switch (sm_conn->sm_irk_lookup_state){
                case IRK_LOOKUP_FAILED:
                    log_info("irk lookup failed, send pairing request");
                    sm_conn->sm_engine_state = SM_INITIATOR_PH1_W2_SEND_PAIRING_REQUEST;
                    break;
                case IRK_LOOKUP_SUCCEEDED:
                        le_device_db_encryption_get(sm_conn->sm_le_db_index, &ediv, NULL, ltk, NULL, NULL, NULL);
                        if (!sm_is_null_key(ltk) || ediv){
                            log_info("sm: Setting up previous ltk/ediv/rand for device index %u", sm_conn->sm_le_db_index);
                            sm_conn->sm_engine_state = SM_INITIATOR_PH0_HAS_LTK;
                        } else {
                            sm_conn->sm_engine_state = SM_INITIATOR_PH1_W2_SEND_PAIRING_REQUEST;
                        }
                        break;
                default:
                    log_info("irk lookup pending");
                    sm_conn->sm_pairing_requested = 1;
                    break;
            }
        } else if (sm_conn->sm_engine_state == SM_GENERAL_IDLE){
            sm_conn->sm_pairing_requested = 1;
        }
    }
    sm_run();
}

// called by client app on authorization request
void sm_authorization_decline(hci_con_handle_t con_handle){
    sm_connection_t * sm_conn = sm_get_connection_for_handle(con_handle);
    if (!sm_conn) return;     // wrong connection
    sm_conn->sm_connection_authorization_state = AUTHORIZATION_DECLINED;
    sm_notify_client_status(SM_EVENT_AUTHORIZATION_RESULT, sm_conn->sm_handle, sm_conn->sm_peer_addr_type, sm_conn->sm_peer_address, 0);
}

void sm_authorization_grant(hci_con_handle_t con_handle){
    sm_connection_t * sm_conn = sm_get_connection_for_handle(con_handle);
    if (!sm_conn) return;     // wrong connection
    sm_conn->sm_connection_authorization_state = AUTHORIZATION_GRANTED;
    sm_notify_client_status(SM_EVENT_AUTHORIZATION_RESULT, sm_conn->sm_handle, sm_conn->sm_peer_addr_type, sm_conn->sm_peer_address, 1);
}

// GAP Bonding API

void sm_bonding_decline(hci_con_handle_t con_handle){
    sm_connection_t * sm_conn = sm_get_connection_for_handle(con_handle);
    if (!sm_conn) return;     // wrong connection
    setup->sm_user_response = SM_USER_RESPONSE_DECLINE;
    log_info("decline, state %u", sm_conn->sm_engine_state);
    switch(sm_conn->sm_engine_state){
#ifdef ENABLE_LE_SECURE_CONNECTIONS
        case SM_SC_W4_USER_RESPONSE:
        case SM_SC_W4_CONFIRMATION:
        case SM_SC_W4_PUBLIC_KEY_COMMAND:
#endif
        case SM_PH1_W4_USER_RESPONSE:
            switch (setup->sm_stk_generation_method){
                case PK_RESP_INPUT:
                case PK_INIT_INPUT:
                case PK_BOTH_INPUT:
                    sm_pairing_error(sm_conn, SM_REASON_PASSKEY_ENTRY_FAILED);
                    break;
                case NUMERIC_COMPARISON:
                    sm_pairing_error(sm_conn, SM_REASON_NUMERIC_COMPARISON_FAILED);
                    break;
                case JUST_WORKS:
                case OOB:
                    sm_pairing_error(sm_conn, SM_REASON_UNSPECIFIED_REASON);
                    break;
            }
            break;
        default:
            break;
    }
    sm_run();
}

void sm_just_works_confirm(hci_con_handle_t con_handle){
    sm_connection_t * sm_conn = sm_get_connection_for_handle(con_handle);
    if (!sm_conn) return;     // wrong connection
    setup->sm_user_response = SM_USER_RESPONSE_CONFIRM;
    if (sm_conn->sm_engine_state == SM_PH1_W4_USER_RESPONSE){
        if (setup->sm_use_secure_connections){
            sm_conn->sm_engine_state = SM_SC_SEND_PUBLIC_KEY_COMMAND;
        } else {
            btstack_crypto_random_generate(&sm_crypto_random_request, setup->sm_local_random, 16, &sm_handle_random_result_ph2_random, sm_conn);
        }
    }

#ifdef ENABLE_LE_SECURE_CONNECTIONS
    if (sm_conn->sm_engine_state == SM_SC_W4_USER_RESPONSE){
        sm_sc_prepare_dhkey_check(sm_conn);
    }
#endif

    sm_run();
}

void sm_numeric_comparison_confirm(hci_con_handle_t con_handle){
    // for now, it's the same
    sm_just_works_confirm(con_handle);
}

void sm_passkey_input(hci_con_handle_t con_handle, uint32_t passkey){
    sm_connection_t * sm_conn = sm_get_connection_for_handle(con_handle);
    if (!sm_conn) return;     // wrong connection
    sm_reset_tk();
    big_endian_store_32(setup->sm_tk, 12, passkey);
    setup->sm_user_response = SM_USER_RESPONSE_PASSKEY;
    if (sm_conn->sm_engine_state == SM_PH1_W4_USER_RESPONSE){
        btstack_crypto_random_generate(&sm_crypto_random_request, setup->sm_local_random, 16, &sm_handle_random_result_ph2_random, sm_conn);
    }
#ifdef ENABLE_LE_SECURE_CONNECTIONS
    memcpy(setup->sm_ra, setup->sm_tk, 16);
    memcpy(setup->sm_rb, setup->sm_tk, 16);
    if (sm_conn->sm_engine_state == SM_SC_W4_USER_RESPONSE){
        sm_sc_start_calculating_local_confirm(sm_conn);
    }
#endif
    sm_run();
}

void sm_keypress_notification(hci_con_handle_t con_handle, uint8_t action){
    sm_connection_t * sm_conn = sm_get_connection_for_handle(con_handle);
    if (!sm_conn) return;     // wrong connection
    if (action > SM_KEYPRESS_PASSKEY_ENTRY_COMPLETED) return;
    uint8_t num_actions = setup->sm_keypress_notification >> 5;
    uint8_t flags = setup->sm_keypress_notification & 0x1f;
    switch (action){
        case SM_KEYPRESS_PASSKEY_ENTRY_STARTED:
        case SM_KEYPRESS_PASSKEY_ENTRY_COMPLETED:
            flags |= (1 << action);
            break;
        case SM_KEYPRESS_PASSKEY_CLEARED:
            // clear counter, keypress & erased flags + set passkey cleared
            flags = (flags & 0x19) | (1 << SM_KEYPRESS_PASSKEY_CLEARED);
            break;
        case SM_KEYPRESS_PASSKEY_DIGIT_ENTERED:
            if (flags & (1 << SM_KEYPRESS_PASSKEY_DIGIT_ERASED)){
                // erase actions queued
                num_actions--;
                if (num_actions == 0){
                    // clear counter, keypress & erased flags
                    flags &= 0x19;
                }
                break;
            }
            num_actions++;
            flags |= (1 << SM_KEYPRESS_PASSKEY_DIGIT_ENTERED);
            break;
        case SM_KEYPRESS_PASSKEY_DIGIT_ERASED:
            if (flags & (1 << SM_KEYPRESS_PASSKEY_DIGIT_ENTERED)){
                // enter actions queued
                num_actions--;
                if (num_actions == 0){
                    // clear counter, keypress & erased flags
                    flags &= 0x19;
                }
                break;
            }
            num_actions++;
            flags |= (1 << SM_KEYPRESS_PASSKEY_DIGIT_ERASED);
            break;
        default:
            break;
    }
    setup->sm_keypress_notification = (num_actions << 5) | flags;
    sm_run();
}

#ifdef ENABLE_LE_SECURE_CONNECTIONS
static void sm_handle_random_result_oob(void * arg){
    UNUSED(arg);
    sm_sc_oob_state = SM_SC_OOB_W2_CALC_CONFIRM;
    sm_run();
}
uint8_t sm_generate_sc_oob_data(void (*callback)(const uint8_t * confirm_value, const uint8_t * random_value)){
    if (sm_sc_oob_state != SM_SC_OOB_IDLE) return ERROR_CODE_COMMAND_DISALLOWED;
    sm_sc_oob_callback = callback;
    sm_sc_oob_state = SM_SC_OOB_W4_RANDOM;
    btstack_crypto_random_generate(&sm_crypto_random_oob_request, sm_sc_oob_random, 16, &sm_handle_random_result_oob, NULL);
    return 0;
}
#endif

/**
 * @brief Identify device in LE Device DB
 * @param handle
 * @returns index from le_device_db or -1 if not found/identified
 */
int sm_le_device_index(hci_con_handle_t con_handle ){
    sm_connection_t * sm_conn = sm_get_connection_for_handle(con_handle);
    if (!sm_conn) return -1;
    return sm_conn->sm_le_db_index;
}

static int gap_random_address_type_requires_updates(void){
    if (gap_random_adress_type == GAP_RANDOM_ADDRESS_TYPE_OFF) return 0;
    if (gap_random_adress_type == GAP_RANDOM_ADDRESS_TYPE_OFF) return 0;
    return 1;
}

static uint8_t own_address_type(void){
    switch (gap_random_adress_type){
        case GAP_RANDOM_ADDRESS_TYPE_OFF:
            return BD_ADDR_TYPE_LE_PUBLIC;
        default:
            return BD_ADDR_TYPE_LE_RANDOM;
    }
}

// GAP LE API
void gap_random_address_set_mode(gap_random_address_type_t random_address_type){
    gap_random_address_update_stop();
    gap_random_adress_type = random_address_type;
    hci_le_set_own_address_type(own_address_type());
    if (!gap_random_address_type_requires_updates()) return;
    gap_random_address_update_start();
    gap_random_address_trigger();
}

gap_random_address_type_t gap_random_address_get_mode(void){
    return gap_random_adress_type;
}

void gap_random_address_set_update_period(int period_ms){
    gap_random_adress_update_period = period_ms;
    if (!gap_random_address_type_requires_updates()) return;
    gap_random_address_update_stop();
    gap_random_address_update_start();
}

void gap_random_address_set(bd_addr_t addr){
    gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_STATIC);
    memcpy(sm_random_address, addr, 6);
    if (rau_state == RAU_W4_WORKING) return;
    rau_state = RAU_SET_ADDRESS;
    sm_run();
}

#ifdef ENABLE_LE_PERIPHERAL
/*
 * @brief Set Advertisement Paramters
 * @param adv_int_min
 * @param adv_int_max
 * @param adv_type
 * @param direct_address_type
 * @param direct_address
 * @param channel_map
 * @param filter_policy
 *
 * @note own_address_type is used from gap_random_address_set_mode
 */
void gap_advertisements_set_params(uint16_t adv_int_min, uint16_t adv_int_max, uint8_t adv_type,
    uint8_t direct_address_typ, bd_addr_t direct_address, uint8_t channel_map, uint8_t filter_policy){
    hci_le_advertisements_set_params(adv_int_min, adv_int_max, adv_type,
        direct_address_typ, direct_address, channel_map, filter_policy);
}
#endif
