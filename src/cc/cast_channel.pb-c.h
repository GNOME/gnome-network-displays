/* Generated by the protocol buffer compiler.  DO NOT EDIT! */
/* Generated from: cast_channel.proto */

#ifndef PROTOBUF_C_cast_5fchannel_2eproto__INCLUDED
#define PROTOBUF_C_cast_5fchannel_2eproto__INCLUDED

#include <protobuf-c/protobuf-c.h>

PROTOBUF_C__BEGIN_DECLS

#if PROTOBUF_C_VERSION_NUMBER < 1000000
# error This file was generated by a newer version of protoc-c which is incompatible with your libprotobuf-c headers. Please update your headers.
#elif 1004001 < PROTOBUF_C_MIN_COMPILER_VERSION
# error This file was generated by an older version of protoc-c which is incompatible with your libprotobuf-c headers. Please regenerate this file with a newer version of protoc-c.
#endif


typedef struct Cast__Channel__CastMessage       Cast__Channel__CastMessage;
typedef struct Cast__Channel__AuthChallenge     Cast__Channel__AuthChallenge;
typedef struct Cast__Channel__AuthResponse      Cast__Channel__AuthResponse;
typedef struct Cast__Channel__AuthError         Cast__Channel__AuthError;
typedef struct Cast__Channel__DeviceAuthMessage Cast__Channel__DeviceAuthMessage;


/* --- enums --- */

/*
 * Always pass a version of the protocol for future compatibility
 * requirements.
 */
typedef enum _Cast__Channel__CastMessage__ProtocolVersion {
  CAST__CHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_0 = 0,
  /*
   * message chunking support (deprecated).
   */
  CAST__CHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_1 = 1,
  /*
   * reworked message chunking.
   */
  CAST__CHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_2 = 2,
  /*
   * binary payload over utf8.
   */
  CAST__CHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_3 = 3
                                                              PROTOBUF_C__FORCE_ENUM_TO_BE_INT_SIZE (CAST__CHANNEL__CAST_MESSAGE__PROTOCOL_VERSION)
} Cast__Channel__CastMessage__ProtocolVersion;
/*
 * What type of data do we have in this message.
 */
typedef enum _Cast__Channel__CastMessage__PayloadType {
  CAST__CHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING = 0,
  CAST__CHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__BINARY = 1
                                                      PROTOBUF_C__FORCE_ENUM_TO_BE_INT_SIZE (CAST__CHANNEL__CAST_MESSAGE__PAYLOAD_TYPE)
} Cast__Channel__CastMessage__PayloadType;
typedef enum _Cast__Channel__AuthError__ErrorType {
  CAST__CHANNEL__AUTH_ERROR__ERROR_TYPE__INTERNAL_ERROR = 0,
  /*
   * The underlying connection is not TLS
   */
  CAST__CHANNEL__AUTH_ERROR__ERROR_TYPE__NO_TLS = 1,
  CAST__CHANNEL__AUTH_ERROR__ERROR_TYPE__SIGNATURE_ALGORITHM_UNAVAILABLE = 2
                                                                           PROTOBUF_C__FORCE_ENUM_TO_BE_INT_SIZE (CAST__CHANNEL__AUTH_ERROR__ERROR_TYPE)
} Cast__Channel__AuthError__ErrorType;
typedef enum _Cast__Channel__SignatureAlgorithm {
  CAST__CHANNEL__SIGNATURE_ALGORITHM__UNSPECIFIED = 0,
  CAST__CHANNEL__SIGNATURE_ALGORITHM__RSASSA_PKCS1v15 = 1,
  CAST__CHANNEL__SIGNATURE_ALGORITHM__RSASSA_PSS = 2
                                                   PROTOBUF_C__FORCE_ENUM_TO_BE_INT_SIZE (CAST__CHANNEL__SIGNATURE_ALGORITHM)
} Cast__Channel__SignatureAlgorithm;
typedef enum _Cast__Channel__HashAlgorithm {
  CAST__CHANNEL__HASH_ALGORITHM__SHA1 = 0,
  CAST__CHANNEL__HASH_ALGORITHM__SHA256 = 1
                                          PROTOBUF_C__FORCE_ENUM_TO_BE_INT_SIZE (CAST__CHANNEL__HASH_ALGORITHM)
} Cast__Channel__HashAlgorithm;

/* --- messages --- */

struct  Cast__Channel__CastMessage
{
  ProtobufCMessage                            base;
  Cast__Channel__CastMessage__ProtocolVersion protocol_version;
  /*
   * source and destination ids identify the origin and destination of the
   * message.  They are used to route messages between endpoints that share a
   * device-to-device channel.
   * For messages between applications:
   *   - The sender application id is a unique identifier generated on behalf of
   *     the sender application.
   *   - The receiver id is always the the session id for the application.
   * For messages to or from the sender or receiver platform, the special ids
   * 'sender-0' and 'receiver-0' can be used.
   * For messages intended for all endpoints using a given channel, the
   * wildcard destination_id '*' can be used.
   */
  char *source_id;
  char *destination_id;
  /*
   * This is the core multiplexing key.  All messages are sent on a namespace
   * and endpoints sharing a channel listen on one or more namespaces.  The
   * namespace defines the protocol and semantics of the message.
   */
  char                                   *namespace_;
  Cast__Channel__CastMessage__PayloadType payload_type;
  /*
   * Depending on payload_type, exactly one of the following optional fields
   * will always be set.
   */
  char               *payload_utf8;
  protobuf_c_boolean  has_payload_binary;
  ProtobufCBinaryData payload_binary;
  /*
   * Flag indicating whether there are more chunks to follow for this message.
   * If the flag is false or is not present, then this is the last (or only)
   * chunk of the message.
   */
  protobuf_c_boolean has_continued;
  protobuf_c_boolean continued;
  /*
   * If this is a chunk of a larger message, and the remaining length of the
   * message payload (the sum of the lengths of the payloads of the remaining
   * chunks) is known, this field will indicate that length. For a given
   * chunked message, this field should either be present in all of the chunks,
   * or in none of them.
   */
  protobuf_c_boolean has_remaining_length;
  uint32_t           remaining_length;
};
#define CAST__CHANNEL__CAST_MESSAGE__INIT \
        { PROTOBUF_C_MESSAGE_INIT (&cast__channel__cast_message__descriptor) \
          , CAST__CHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_0, NULL, NULL, NULL, CAST__CHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING, NULL, 0, {0, NULL}, 0, 0, 0, 0 }


/*
 * Messages for authentication protocol between a sender and a receiver.
 */
struct  Cast__Channel__AuthChallenge
{
  ProtobufCMessage                  base;
  protobuf_c_boolean                has_signature_algorithm;
  Cast__Channel__SignatureAlgorithm signature_algorithm;
  protobuf_c_boolean                has_sender_nonce;
  ProtobufCBinaryData               sender_nonce;
  protobuf_c_boolean                has_hash_algorithm;
  Cast__Channel__HashAlgorithm      hash_algorithm;
};
#define CAST__CHANNEL__AUTH_CHALLENGE__INIT \
        { PROTOBUF_C_MESSAGE_INIT (&cast__channel__auth_challenge__descriptor) \
          , 0, CAST__CHANNEL__SIGNATURE_ALGORITHM__RSASSA_PKCS1v15, 0, {0, NULL}, 0, CAST__CHANNEL__HASH_ALGORITHM__SHA1 }


struct  Cast__Channel__AuthResponse
{
  ProtobufCMessage                  base;
  ProtobufCBinaryData               signature;
  ProtobufCBinaryData               client_auth_certificate;
  size_t                            n_intermediate_certificate;
  ProtobufCBinaryData              *intermediate_certificate;
  protobuf_c_boolean                has_signature_algorithm;
  Cast__Channel__SignatureAlgorithm signature_algorithm;
  protobuf_c_boolean                has_sender_nonce;
  ProtobufCBinaryData               sender_nonce;
  protobuf_c_boolean                has_hash_algorithm;
  Cast__Channel__HashAlgorithm      hash_algorithm;
  protobuf_c_boolean                has_crl;
  ProtobufCBinaryData               crl;
};
#define CAST__CHANNEL__AUTH_RESPONSE__INIT \
        { PROTOBUF_C_MESSAGE_INIT (&cast__channel__auth_response__descriptor) \
          , {0, NULL}, {0, NULL}, 0, NULL, 0, CAST__CHANNEL__SIGNATURE_ALGORITHM__RSASSA_PKCS1v15, 0, {0, NULL}, 0, CAST__CHANNEL__HASH_ALGORITHM__SHA1, 0, {0, NULL} }


struct  Cast__Channel__AuthError
{
  ProtobufCMessage                    base;
  Cast__Channel__AuthError__ErrorType error_type;
};
#define CAST__CHANNEL__AUTH_ERROR__INIT \
        { PROTOBUF_C_MESSAGE_INIT (&cast__channel__auth_error__descriptor) \
          , CAST__CHANNEL__AUTH_ERROR__ERROR_TYPE__INTERNAL_ERROR }


struct  Cast__Channel__DeviceAuthMessage
{
  ProtobufCMessage base;
  /*
   * Request fields
   */
  Cast__Channel__AuthChallenge *challenge;
  /*
   * Response fields
   */
  Cast__Channel__AuthResponse *response;
  Cast__Channel__AuthError    *error;
};
#define CAST__CHANNEL__DEVICE_AUTH_MESSAGE__INIT \
        { PROTOBUF_C_MESSAGE_INIT (&cast__channel__device_auth_message__descriptor) \
          , NULL, NULL, NULL }


/* Cast__Channel__CastMessage methods */
void   cast__channel__cast_message__init (Cast__Channel__CastMessage *message);
size_t cast__channel__cast_message__get_packed_size (const Cast__Channel__CastMessage *message);
size_t cast__channel__cast_message__pack (const Cast__Channel__CastMessage *message,
                                          uint8_t                          *out);
size_t cast__channel__cast_message__pack_to_buffer (const Cast__Channel__CastMessage *message,
                                                    ProtobufCBuffer                  *buffer);
Cast__Channel__CastMessage *cast__channel__cast_message__unpack (ProtobufCAllocator *allocator,
                                                                 size_t              len,
                                                                 const uint8_t      *data);
void   cast__channel__cast_message__free_unpacked (Cast__Channel__CastMessage *message,
                                                   ProtobufCAllocator         *allocator);
/* Cast__Channel__AuthChallenge methods */
void   cast__channel__auth_challenge__init (Cast__Channel__AuthChallenge *message);
size_t cast__channel__auth_challenge__get_packed_size (const Cast__Channel__AuthChallenge *message);
size_t cast__channel__auth_challenge__pack (const Cast__Channel__AuthChallenge *message,
                                            uint8_t                            *out);
size_t cast__channel__auth_challenge__pack_to_buffer (const Cast__Channel__AuthChallenge *message,
                                                      ProtobufCBuffer                    *buffer);
Cast__Channel__AuthChallenge *cast__channel__auth_challenge__unpack (ProtobufCAllocator *allocator,
                                                                     size_t              len,
                                                                     const uint8_t      *data);
void   cast__channel__auth_challenge__free_unpacked (Cast__Channel__AuthChallenge *message,
                                                     ProtobufCAllocator           *allocator);
/* Cast__Channel__AuthResponse methods */
void   cast__channel__auth_response__init (Cast__Channel__AuthResponse *message);
size_t cast__channel__auth_response__get_packed_size (const Cast__Channel__AuthResponse *message);
size_t cast__channel__auth_response__pack (const Cast__Channel__AuthResponse *message,
                                           uint8_t                           *out);
size_t cast__channel__auth_response__pack_to_buffer (const Cast__Channel__AuthResponse *message,
                                                     ProtobufCBuffer                   *buffer);
Cast__Channel__AuthResponse *cast__channel__auth_response__unpack (ProtobufCAllocator *allocator,
                                                                   size_t              len,
                                                                   const uint8_t      *data);
void   cast__channel__auth_response__free_unpacked (Cast__Channel__AuthResponse *message,
                                                    ProtobufCAllocator          *allocator);
/* Cast__Channel__AuthError methods */
void   cast__channel__auth_error__init (Cast__Channel__AuthError *message);
size_t cast__channel__auth_error__get_packed_size (const Cast__Channel__AuthError *message);
size_t cast__channel__auth_error__pack (const Cast__Channel__AuthError *message,
                                        uint8_t                        *out);
size_t cast__channel__auth_error__pack_to_buffer (const Cast__Channel__AuthError *message,
                                                  ProtobufCBuffer                *buffer);
Cast__Channel__AuthError *cast__channel__auth_error__unpack (ProtobufCAllocator *allocator,
                                                             size_t              len,
                                                             const uint8_t      *data);
void   cast__channel__auth_error__free_unpacked (Cast__Channel__AuthError *message,
                                                 ProtobufCAllocator       *allocator);
/* Cast__Channel__DeviceAuthMessage methods */
void   cast__channel__device_auth_message__init (Cast__Channel__DeviceAuthMessage *message);
size_t cast__channel__device_auth_message__get_packed_size (const Cast__Channel__DeviceAuthMessage *message);
size_t cast__channel__device_auth_message__pack (const Cast__Channel__DeviceAuthMessage *message,
                                                 uint8_t                                *out);
size_t cast__channel__device_auth_message__pack_to_buffer (const Cast__Channel__DeviceAuthMessage *message,
                                                           ProtobufCBuffer                        *buffer);
Cast__Channel__DeviceAuthMessage *cast__channel__device_auth_message__unpack (ProtobufCAllocator *allocator,
                                                                              size_t              len,
                                                                              const uint8_t      *data);
void   cast__channel__device_auth_message__free_unpacked (Cast__Channel__DeviceAuthMessage *message,
                                                          ProtobufCAllocator               *allocator);
/* --- per-message closures --- */

typedef void (*Cast__Channel__CastMessage_Closure)(const Cast__Channel__CastMessage *message,
                                                   void                             *closure_data);
typedef void (*Cast__Channel__AuthChallenge_Closure)(const Cast__Channel__AuthChallenge *message,
                                                     void                               *closure_data);
typedef void (*Cast__Channel__AuthResponse_Closure)(const Cast__Channel__AuthResponse *message,
                                                    void                              *closure_data);
typedef void (*Cast__Channel__AuthError_Closure)(const Cast__Channel__AuthError *message,
                                                 void                           *closure_data);
typedef void (*Cast__Channel__DeviceAuthMessage_Closure)(const Cast__Channel__DeviceAuthMessage *message,
                                                         void                                   *closure_data);

/* --- services --- */


/* --- descriptors --- */

extern const ProtobufCEnumDescriptor cast__channel__signature_algorithm__descriptor;
extern const ProtobufCEnumDescriptor cast__channel__hash_algorithm__descriptor;
extern const ProtobufCMessageDescriptor cast__channel__cast_message__descriptor;
extern const ProtobufCEnumDescriptor cast__channel__cast_message__protocol_version__descriptor;
extern const ProtobufCEnumDescriptor cast__channel__cast_message__payload_type__descriptor;
extern const ProtobufCMessageDescriptor cast__channel__auth_challenge__descriptor;
extern const ProtobufCMessageDescriptor cast__channel__auth_response__descriptor;
extern const ProtobufCMessageDescriptor cast__channel__auth_error__descriptor;
extern const ProtobufCEnumDescriptor cast__channel__auth_error__error_type__descriptor;
extern const ProtobufCMessageDescriptor cast__channel__device_auth_message__descriptor;

PROTOBUF_C__END_DECLS


#endif  /* PROTOBUF_C_cast_5fchannel_2eproto__INCLUDED */
