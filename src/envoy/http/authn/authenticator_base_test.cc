/* Copyright 2018 Istio Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/envoy/http/authn/authenticator_base.h"
#include "common/common/base64.h"
#include "common/protobuf/protobuf.h"
#include "envoy/config/filter/http/authn/v2alpha1/config.pb.h"
#include "gmock/gmock.h"
#include "src/envoy/http/authn/test_utils.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/ssl/mocks.h"

using google::protobuf::util::MessageDifferencer;
using istio::authn::Payload;
using istio::envoy::config::filter::http::authn::v2alpha1::FilterConfig;
using testing::NiceMock;
using testing::Return;

namespace iaapi = istio::authentication::v1alpha1;

namespace Envoy {
namespace Http {
namespace Istio {
namespace AuthN {
namespace {

const std::string kSecIstioAuthUserInfoHeaderKey = "sec-istio-auth-userinfo";
const std::string kSecIstioAuthUserinfoHeaderValue =
    R"(
     {
       "iss": "issuer@foo.com",
       "sub": "sub@foo.com",
       "aud": "aud1",
       "non-string-will-be-ignored": 1512754205,
       "some-other-string-claims": "some-claims-kept"
     }
   )";

class MockAuthenticatorBase : public AuthenticatorBase {
 public:
  MockAuthenticatorBase(FilterContext* filter_context)
      : AuthenticatorBase(filter_context) {}
  MOCK_METHOD1(run, bool(Payload*));
};

class AuthenticatorBaseTest : public testing::Test,
                              public Logger::Loggable<Logger::Id::filter> {
 public:
  virtual ~AuthenticatorBaseTest() {}

  Http::TestHeaderMapImpl request_headers_{};
  NiceMock<Envoy::Network::MockConnection> connection_{};
  NiceMock<Envoy::Ssl::MockConnection> ssl_{};
  FilterConfig filter_config_{};
  FilterContext filter_context_{&request_headers_, &connection_,
                                istio::envoy::config::filter::http::authn::
                                    v2alpha1::FilterConfig::default_instance()};

  MockAuthenticatorBase authenticator_{&filter_context_};

  void SetUp() override { payload_ = new Payload(); }

  void TearDown() override { delete (payload_); }

 protected:
  iaapi::MutualTls mtls_params_;
  iaapi::Jwt jwt_;
  Payload* payload_;
  Payload default_payload_;
};

Http::TestHeaderMapImpl CreateTestHeaderMap(const std::string& header_key,
                                            const std::string& header_value) {
  // The base64 encoding is done through Base64::encode().
  // If the test input has special chars, may need to use the counterpart of
  // Base64UrlDecode().
  std::string value_base64 =
      Base64::encode(header_value.c_str(), header_value.size());
  return Http::TestHeaderMapImpl{{header_key, value_base64}};
}

TEST_F(AuthenticatorBaseTest, ValidateMtlsOnPlaintextConnection) {
  EXPECT_FALSE(authenticator_.validateX509(mtls_params_, payload_));
  EXPECT_TRUE(MessageDifferencer::Equals(*payload_, default_payload_));
}

TEST_F(AuthenticatorBaseTest, ValidateTlsOnPlaintextConnection) {
  mtls_params_.set_allow_tls(true);  // allow TLS connection
  // When requiring TLS, plaintext connection should fail.
  EXPECT_FALSE(authenticator_.validateX509(mtls_params_, payload_));
  EXPECT_TRUE(MessageDifferencer::Equals(*payload_, default_payload_));
}

TEST_F(AuthenticatorBaseTest, ValidateMtlsOnSslConnectionWithNoPeerCert) {
  EXPECT_CALL(Const(connection_), ssl()).WillRepeatedly(Return(&ssl_));
  EXPECT_CALL(Const(ssl_), peerCertificatePresented())
      .Times(1)
      .WillOnce(Return(false));

  EXPECT_FALSE(authenticator_.validateX509(mtls_params_, payload_));
  EXPECT_TRUE(MessageDifferencer::Equals(*payload_, default_payload_));
}

TEST_F(AuthenticatorBaseTest, ValidateTlsOnSslConnectionWithNoPeerCert) {
  mtls_params_.set_allow_tls(true);  // allow TLS connection
  EXPECT_CALL(Const(connection_), ssl()).WillRepeatedly(Return(&ssl_));
  EXPECT_CALL(Const(ssl_), peerCertificatePresented())
      .Times(1)
      .WillOnce(Return(false));

  // When client certificate is not present on TLS, authentication should still
  // succeed.
  EXPECT_TRUE(authenticator_.validateX509(mtls_params_, payload_));
}

TEST_F(AuthenticatorBaseTest, ValidateMtlsOnSslConnectionWithPeerCert) {
  EXPECT_CALL(Const(connection_), ssl()).WillRepeatedly(Return(&ssl_));
  EXPECT_CALL(Const(ssl_), peerCertificatePresented())
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(ssl_, uriSanPeerCertificate()).Times(1).WillOnce(Return("foo"));
  EXPECT_TRUE(authenticator_.validateX509(mtls_params_, payload_));
  // When client certificate is present on mTLS, authenticated attribute should
  // be extracted.
  EXPECT_EQ(payload_->x509().user(), "foo");
}

TEST_F(AuthenticatorBaseTest, ValidateTlsOnSslConnectionWithPeerCert) {
  mtls_params_.set_allow_tls(true);  // allow TLS connection
  EXPECT_CALL(Const(connection_), ssl()).WillRepeatedly(Return(&ssl_));
  EXPECT_CALL(Const(ssl_), peerCertificatePresented())
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(ssl_, uriSanPeerCertificate()).Times(1).WillOnce(Return("foo"));
  EXPECT_TRUE(authenticator_.validateX509(mtls_params_, payload_));
  // When client certificate is present on TLS, authenticated attribute should
  // be extracted.
  EXPECT_EQ(payload_->x509().user(), "foo");
}

TEST_F(AuthenticatorBaseTest, ValidateMtlsOnSslConnectionWithPeerSpiffeCert) {
  EXPECT_CALL(Const(connection_), ssl()).WillRepeatedly(Return(&ssl_));
  EXPECT_CALL(Const(ssl_), peerCertificatePresented())
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(ssl_, uriSanPeerCertificate())
      .Times(1)
      .WillOnce(Return("spiffe://foo"));
  EXPECT_TRUE(authenticator_.validateX509(mtls_params_, payload_));

  // When client certificate is present on mTLS, authenticated attribute should
  // be extracted.
  EXPECT_EQ(payload_->x509().user(), "foo");
}

TEST_F(AuthenticatorBaseTest, ValidateTlsOnSslConnectionWithPeerSpiffeCert) {
  mtls_params_.set_allow_tls(true);  // allow TLS connection
  EXPECT_CALL(Const(connection_), ssl()).WillRepeatedly(Return(&ssl_));
  EXPECT_CALL(Const(ssl_), peerCertificatePresented())
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(ssl_, uriSanPeerCertificate())
      .Times(1)
      .WillOnce(Return("spiffe://foo"));
  EXPECT_TRUE(authenticator_.validateX509(mtls_params_, payload_));
  // When client certificate is present on TLS, authenticated attribute should
  // be extracted.
  EXPECT_EQ(payload_->x509().user(), "foo");
}

TEST_F(AuthenticatorBaseTest,
       ValidateMtlsOnSslConnectionWithPeerMalformedSpiffeCert) {
  EXPECT_CALL(Const(connection_), ssl()).WillRepeatedly(Return(&ssl_));
  EXPECT_CALL(Const(ssl_), peerCertificatePresented())
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(ssl_, uriSanPeerCertificate())
      .Times(1)
      .WillOnce(Return("spiffe:foo"));
  EXPECT_TRUE(authenticator_.validateX509(mtls_params_, payload_));

  // When client certificate is present on mTLS and the spiffe subject format is
  // wrong
  // ("spiffe:foo" instead of "spiffe://foo"), the user attribute should be
  // extracted.
  EXPECT_EQ(payload_->x509().user(), "spiffe:foo");
}

TEST_F(AuthenticatorBaseTest,
       ValidateTlsOnSslConnectionWithPeerMalformedSpiffeCert) {
  mtls_params_.set_allow_tls(true);  // allow TLS connection
  EXPECT_CALL(Const(connection_), ssl()).WillRepeatedly(Return(&ssl_));
  EXPECT_CALL(Const(ssl_), peerCertificatePresented())
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(ssl_, uriSanPeerCertificate())
      .Times(1)
      .WillOnce(Return("spiffe:foo"));
  EXPECT_TRUE(authenticator_.validateX509(mtls_params_, payload_));
  // When client certificate is present on TLS and the spiffe subject format is
  // wrong
  // ("spiffe:foo" instead of "spiffe://foo"), the user attribute should be
  // extracted.
  EXPECT_EQ(payload_->x509().user(), "spiffe:foo");
}

// TODO: more tests for Jwt.
TEST_F(AuthenticatorBaseTest, ValidateJwtWithNoIstioAuthnConfig) {
  jwt_.set_issuer("issuer@foo.com");
  // authenticator_ has empty Istio authn config
  // When there is empty Istio authn config, validateJwt() should return
  // nullptr and failure.
  EXPECT_FALSE(authenticator_.validateJwt(jwt_, payload_));
  EXPECT_TRUE(MessageDifferencer::Equals(*payload_, default_payload_));
}

TEST_F(AuthenticatorBaseTest, ValidateJwtWithNoIssuer) {
  // no issuer in jwt
  google::protobuf::util::JsonParseOptions options;
  FilterConfig filter_config;
  JsonStringToMessage(
      R"({
              "jwt_output_payload_locations":
              {
                "issuer@foo.com": "sec-istio-auth-userinfo"
              }
           }
        )",
      &filter_config, options);
  Http::TestHeaderMapImpl empty_request_headers{};
  FilterContext filter_context{&empty_request_headers, &connection_,
                               filter_config};
  MockAuthenticatorBase authenticator{&filter_context};

  // When there is no issuer in the JWT config, validateJwt() should return
  // nullptr and failure.
  EXPECT_FALSE(authenticator_.validateJwt(jwt_, payload_));
  EXPECT_TRUE(MessageDifferencer::Equals(*payload_, default_payload_));
}

TEST_F(AuthenticatorBaseTest, ValidateJwtWithEmptyJwtOutputPayloadLocations) {
  jwt_.set_issuer("issuer@foo.com");
  Http::TestHeaderMapImpl request_headers_with_jwt = CreateTestHeaderMap(
      kSecIstioAuthUserInfoHeaderKey, kSecIstioAuthUserinfoHeaderValue);
  google::protobuf::util::JsonParseOptions options;
  FilterConfig filter_config;
  JsonStringToMessage(
      R"({
              "jwt_output_payload_locations":
              {
              }
           }
        )",
      &filter_config, options);
  FilterContext filter_context{&request_headers_with_jwt, &connection_,
                               filter_config};
  MockAuthenticatorBase authenticator{&filter_context};
  // authenticator has empty jwt_output_payload_locations in Istio authn config
  // When there is no matching jwt_output_payload_locations for the issuer in
  // the Istio authn config, validateJwt() should return nullptr and failure.
  EXPECT_FALSE(authenticator_.validateJwt(jwt_, payload_));
  EXPECT_TRUE(MessageDifferencer::Equals(*payload_, default_payload_));
}

TEST_F(AuthenticatorBaseTest, ValidateJwtWithNoJwtInHeader) {
  jwt_.set_issuer("issuer@foo.com");
  google::protobuf::util::JsonParseOptions options;
  FilterConfig filter_config;
  JsonStringToMessage(
      R"({
              "jwt_output_payload_locations":
              {
                "issuer@foo.com": "sec-istio-auth-jwt-output"
              }
           }
        )",
      &filter_config, options);
  Http::TestHeaderMapImpl empty_request_headers{};
  FilterContext filter_context{&empty_request_headers, &connection_,
                               filter_config};
  MockAuthenticatorBase authenticator{&filter_context};
  // When there is no JWT in the HTTP header, validateJwt() should return
  // nullptr and failure.
  EXPECT_FALSE(authenticator_.validateJwt(jwt_, payload_));
  EXPECT_TRUE(MessageDifferencer::Equals(*payload_, default_payload_));
}

TEST_F(AuthenticatorBaseTest, ValidateJwtWithJwtInHeader) {
  jwt_.set_issuer("issuer@foo.com");
  Http::TestHeaderMapImpl request_headers_with_jwt = CreateTestHeaderMap(
      "sec-istio-auth-jwt-output", kSecIstioAuthUserinfoHeaderValue);
  google::protobuf::util::JsonParseOptions options;
  FilterConfig filter_config;
  JsonStringToMessage(
      R"({
              "jwt_output_payload_locations":
              {
                "issuer@foo.com": "sec-istio-auth-jwt-output"
              }
           }
        )",
      &filter_config, options);
  FilterContext filter_context{&request_headers_with_jwt, &connection_,
                               filter_config};
  MockAuthenticatorBase authenticator{&filter_context};
  Payload expected_payload;
  JsonStringToMessage(
      R"({
             "jwt": {
               "user": "issuer@foo.com/sub@foo.com",
               "audiences": ["aud1"],
               "presenter": "",
               "claims": {
                 "aud": "aud1",
                 "iss": "issuer@foo.com",
                 "sub": "sub@foo.com",
                 "some-other-string-claims": "some-claims-kept"
               }
             }
           }
        )",
      &expected_payload, options);

  EXPECT_TRUE(authenticator.validateJwt(jwt_, payload_));
  EXPECT_TRUE(MessageDifferencer::Equals(expected_payload, *payload_));
}

}  // namespace
}  // namespace AuthN
}  // namespace Istio
}  // namespace Http
}  // namespace Envoy
