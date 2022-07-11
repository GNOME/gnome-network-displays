/* Generated by the protocol buffer compiler.  DO NOT EDIT! */
/* Generated from: cast_channel.proto */

#ifndef PROTOBUF_C_cast_5fchannel_2eproto__INCLUDED
#define PROTOBUF_C_cast_5fchannel_2eproto__INCLUDED

#include <protobuf-c/protobuf-c.h>

PROTOBUF_C__BEGIN_DECLS

#if PROTOBUF_C_VERSION_NUMBER < 1000000
# error This file was generated by a newer version of protoc-c which is incompatible with your libprotobuf-c headers. Please update your headers.
#elif 1004000 < PROTOBUF_C_MIN_COMPILER_VERSION
# error This file was generated by an older version of protoc-c which is incompatible with your libprotobuf-c headers. Please regenerate this file with a newer version of protoc-c.
#endif


typedef struct Castchannel__CastMessage Castchannel__CastMessage;
typedef struct Castchannel__AuthChallenge Castchannel__AuthChallenge;
typedef struct Castchannel__AuthResponse Castchannel__AuthResponse;
typedef struct Castchannel__AuthError Castchannel__AuthError;
typedef struct Castchannel__DeviceAuthMessage Castchannel__DeviceAuthMessage;


/* --- enums --- */

/*
 * Always pass a version of the protocol for future compatibility
 * requirements.
 */
typedef enum _Castchannel__CastMessage__ProtocolVersion {
  CASTCHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_0 = 0
    PROTOBUF_C__FORCE_ENUM_TO_BE_INT_SIZE(CASTCHANNEL__CAST_MESSAGE__PROTOCOL_VERSION)
} Castchannel__CastMessage__ProtocolVersion;
/*
 * What type of data do we have in this message.
 */
typedef enum _Castchannel__CastMessage__PayloadType {
  CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING = 0,
  CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__BINARY = 1
    PROTOBUF_C__FORCE_ENUM_TO_BE_INT_SIZE(CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE)
} Castchannel__CastMessage__PayloadType;
typedef enum _Castchannel__AuthError__ErrorType {
  CASTCHANNEL__AUTH_ERROR__ERROR_TYPE__INTERNAL_ERROR = 0,
  /*
   * The underlying connection is not TLS
   */
  CASTCHANNEL__AUTH_ERROR__ERROR_TYPE__NO_TLS = 1
    PROTOBUF_C__FORCE_ENUM_TO_BE_INT_SIZE(CASTCHANNEL__AUTH_ERROR__ERROR_TYPE)
} Castchannel__AuthError__ErrorType;

/* --- messages --- */

struct  Castchannel__CastMessage
{
  ProtobufCMessage base;
  Castchannel__CastMessage__ProtocolVersion protocol_version;
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
  char *namespace_;
  Castchannel__CastMessage__PayloadType payload_type;
  /*
   * Depending on payload_type, exactly one of the following optional fields
   * will always be set.
   */
  char *payload_utf8;
  protobuf_c_boolean has_payload_binary;
  ProtobufCBinaryData payload_binary;
};
#define CASTCHANNEL__CAST_MESSAGE__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&castchannel__cast_message__descriptor) \
    , CASTCHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_0, NULL, NULL, NULL, CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING, NULL, 0, {0,NULL} }


/*
 * Messages for authentication protocol between a sender and a receiver.
 */
struct  Castchannel__AuthChallenge
{
  ProtobufCMessage base;
};
#define CASTCHANNEL__AUTH_CHALLENGE__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&castchannel__auth_challenge__descriptor) \
     }


struct  Castchannel__AuthResponse
{
  ProtobufCMessage base;
  ProtobufCBinaryData signature;
  ProtobufCBinaryData client_auth_certificate;
};
#define CASTCHANNEL__AUTH_RESPONSE__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&castchannel__auth_response__descriptor) \
    , {0,NULL}, {0,NULL} }


struct  Castchannel__AuthError
{
  ProtobufCMessage base;
  Castchannel__AuthError__ErrorType error_type;
};
#define CASTCHANNEL__AUTH_ERROR__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&castchannel__auth_error__descriptor) \
    , CASTCHANNEL__AUTH_ERROR__ERROR_TYPE__INTERNAL_ERROR }


struct  Castchannel__DeviceAuthMessage
{
  ProtobufCMessage base;
  /*
   * Request fields
   */
  Castchannel__AuthChallenge *challenge;
  /*
   * Response fields
   */
  Castchannel__AuthResponse *response;
  Castchannel__AuthError *error;
};
#define CASTCHANNEL__DEVICE_AUTH_MESSAGE__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&castchannel__device_auth_message__descriptor) \
    , NULL, NULL, NULL }


/* Castchannel__CastMessage methods */
void   castchannel__cast_message__init
                     (Castchannel__CastMessage         *message);
size_t castchannel__cast_message__get_packed_size
                     (const Castchannel__CastMessage   *message);
size_t castchannel__cast_message__pack
                     (const Castchannel__CastMessage   *message,
                      uint8_t             *out);
size_t castchannel__cast_message__pack_to_buffer
                     (const Castchannel__CastMessage   *message,
                      ProtobufCBuffer     *buffer);
Castchannel__CastMessage *
       castchannel__cast_message__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   castchannel__cast_message__free_unpacked
                     (Castchannel__CastMessage *message,
                      ProtobufCAllocator *allocator);
/* Castchannel__AuthChallenge methods */
void   castchannel__auth_challenge__init
                     (Castchannel__AuthChallenge         *message);
size_t castchannel__auth_challenge__get_packed_size
                     (const Castchannel__AuthChallenge   *message);
size_t castchannel__auth_challenge__pack
                     (const Castchannel__AuthChallenge   *message,
                      uint8_t             *out);
size_t castchannel__auth_challenge__pack_to_buffer
                     (const Castchannel__AuthChallenge   *message,
                      ProtobufCBuffer     *buffer);
Castchannel__AuthChallenge *
       castchannel__auth_challenge__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   castchannel__auth_challenge__free_unpacked
                     (Castchannel__AuthChallenge *message,
                      ProtobufCAllocator *allocator);
/* Castchannel__AuthResponse methods */
void   castchannel__auth_response__init
                     (Castchannel__AuthResponse         *message);
size_t castchannel__auth_response__get_packed_size
                     (const Castchannel__AuthResponse   *message);
size_t castchannel__auth_response__pack
                     (const Castchannel__AuthResponse   *message,
                      uint8_t             *out);
size_t castchannel__auth_response__pack_to_buffer
                     (const Castchannel__AuthResponse   *message,
                      ProtobufCBuffer     *buffer);
Castchannel__AuthResponse *
       castchannel__auth_response__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   castchannel__auth_response__free_unpacked
                     (Castchannel__AuthResponse *message,
                      ProtobufCAllocator *allocator);
/* Castchannel__AuthError methods */
void   castchannel__auth_error__init
                     (Castchannel__AuthError         *message);
size_t castchannel__auth_error__get_packed_size
                     (const Castchannel__AuthError   *message);
size_t castchannel__auth_error__pack
                     (const Castchannel__AuthError   *message,
                      uint8_t             *out);
size_t castchannel__auth_error__pack_to_buffer
                     (const Castchannel__AuthError   *message,
                      ProtobufCBuffer     *buffer);
Castchannel__AuthError *
       castchannel__auth_error__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   castchannel__auth_error__free_unpacked
                     (Castchannel__AuthError *message,
                      ProtobufCAllocator *allocator);
/* Castchannel__DeviceAuthMessage methods */
void   castchannel__device_auth_message__init
                     (Castchannel__DeviceAuthMessage         *message);
size_t castchannel__device_auth_message__get_packed_size
                     (const Castchannel__DeviceAuthMessage   *message);
size_t castchannel__device_auth_message__pack
                     (const Castchannel__DeviceAuthMessage   *message,
                      uint8_t             *out);
size_t castchannel__device_auth_message__pack_to_buffer
                     (const Castchannel__DeviceAuthMessage   *message,
                      ProtobufCBuffer     *buffer);
Castchannel__DeviceAuthMessage *
       castchannel__device_auth_message__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   castchannel__device_auth_message__free_unpacked
                     (Castchannel__DeviceAuthMessage *message,
                      ProtobufCAllocator *allocator);
/* --- per-message closures --- */

typedef void (*Castchannel__CastMessage_Closure)
                 (const Castchannel__CastMessage *message,
                  void *closure_data);
typedef void (*Castchannel__AuthChallenge_Closure)
                 (const Castchannel__AuthChallenge *message,
                  void *closure_data);
typedef void (*Castchannel__AuthResponse_Closure)
                 (const Castchannel__AuthResponse *message,
                  void *closure_data);
typedef void (*Castchannel__AuthError_Closure)
                 (const Castchannel__AuthError *message,
                  void *closure_data);
typedef void (*Castchannel__DeviceAuthMessage_Closure)
                 (const Castchannel__DeviceAuthMessage *message,
                  void *closure_data);

/* --- services --- */


/* --- descriptors --- */

extern const ProtobufCMessageDescriptor castchannel__cast_message__descriptor;
extern const ProtobufCEnumDescriptor    castchannel__cast_message__protocol_version__descriptor;
extern const ProtobufCEnumDescriptor    castchannel__cast_message__payload_type__descriptor;
extern const ProtobufCMessageDescriptor castchannel__auth_challenge__descriptor;
extern const ProtobufCMessageDescriptor castchannel__auth_response__descriptor;
extern const ProtobufCMessageDescriptor castchannel__auth_error__descriptor;
extern const ProtobufCEnumDescriptor    castchannel__auth_error__error_type__descriptor;
extern const ProtobufCMessageDescriptor castchannel__device_auth_message__descriptor;

PROTOBUF_C__END_DECLS


#endif  /* PROTOBUF_C_cast_5fchannel_2eproto__INCLUDED */
