#include "AccessKeyFetcher.h"

#include <cstring>           // for strrchr
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <type_traits>       // for remove_extent_t
#include <vector>            // for vector

#include "BellLogger.h"    // for AbstractLogger
#include "BellUtils.h"     // for BELL_SLEEP_MS
#include "CSpotContext.h"  // for Context
#include "HTTPClient.h"
#include "Logger.h"            // for CSPOT_LOG
#include "MercurySession.h"    // for MercurySession, MercurySession::Res...
#include "NanoPBExtensions.h"  // for bell::nanopb::encode...
#include "NanoPBHelper.h"      // for pbEncode and pbDecode
#include "Packet.h"            // for cspot
#include "TimeProvider.h"      // for TimeProvider
#include "Utils.h"             // for string_format

#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

// set when Spotify rejects our client id/secret, cleared on success (read from C)
extern "C" {
volatile int cspot_credentials_rejected = 0;
void (*cspot_credentials_cb)(int rejected) = nullptr;
}

static void setCredentialsRejected(int rejected) {
  if (rejected == cspot_credentials_rejected) return;
  cspot_credentials_rejected = rejected;
  if (cspot_credentials_cb) cspot_credentials_cb(rejected);
}

static std::string SCOPES =
    "streaming,user-library-read,user-library-modify,user-top-read,user-read-"
    "recently-played";  // Required access scopes

AccessKeyFetcher::AccessKeyFetcher(std::shared_ptr<cspot::Context> ctx)
    : ctx(ctx) {}

bool AccessKeyFetcher::isExpired() {
  if (accessKey.empty()) {
    return true;
  }

  if (ctx->timeProvider->getSyncedTimestamp() > expiresAt) {
    return true;
  }

  return false;
}

std::string AccessKeyFetcher::getAccessKey() {
  if (!isExpired()) {
    return accessKey;
  }

  updateAccessKey();

  return accessKey;
}

void AccessKeyFetcher::updateAccessKey() {
  if (keyPending) {
    // Already pending refresh request
    return;
  }

  keyPending = true;

  // Max retry of 3, can receive different hash cat types
  int retryCount = 3;
  bool success = false;

  do {
    CSPOT_LOG(info, "Access token expired, fetching new one...");

    auto credentials = "grant_type=client_credentials&client_id=" + ctx->config.clientId + "&client_secret=" + ctx->config.clientSecret;
    std::vector<uint8_t> body(credentials.begin(), credentials.end());

#ifdef BELL_ONLY_CJSON
    // a failed request or an unexpected answer must not take the whole player down
    cJSON* root = nullptr;
    try {
      auto response = bell::HTTPClient::post(
          "https://accounts.spotify.com/api/token",
          { {"Content-Type", "application/x-www-form-urlencoded"} }, body);
      root = cJSON_Parse(std::string(response->body()).c_str());
    } catch (const std::exception& e) {
      CSPOT_LOG(error, "Access token request failed: %s", e.what());
    }

    cJSON* token = cJSON_GetObjectItem(root, "access_token");
    cJSON* expires = cJSON_GetObjectItem(root, "expires_in");
    cJSON* error = cJSON_GetObjectItem(root, "error");

    bool valid = cJSON_IsString(token) && cJSON_IsNumber(expires);

    if (!valid) {
      // invalid_client means the client id/secret are wrong, revoked or rotated
      if (cJSON_IsString(error) && (!strcmp(error->valuestring, "invalid_client") ||
                                    !strcmp(error->valuestring, "unauthorized_client"))) {
        setCredentialsRejected(1);
      }
      CSPOT_LOG(error, "Access token error: %s", cJSON_IsString(error) ? error->valuestring : "no valid answer");
      cJSON_Delete(root);
    }

    if (valid) {
        accessKey = std::string(token->valuestring);
        int expiresIn = expires->valueint;
        cJSON_Delete(root);
        setCredentialsRejected(0);
#else
    auto response = bell::HTTPClient::post(
        "https://accounts.spotify.com/api/token",
        { {"Content-Type", "application/x-www-form-urlencoded"} }, body);

    auto root = nlohmann::json::parse(response->bytes());
    if (!root.contains("error")) {
        accessKey = std::string(root["access_token"]);
        int expiresIn = root["expires_in"];
#endif
        // Successfully received an auth token
      CSPOT_LOG(info, "Access token sucessfully fetched");
      success = true;

      this->expiresAt =
            ctx->timeProvider->getSyncedTimestamp() + (expiresIn * 1000);
    }
    else {
      CSPOT_LOG(error, "Failed to fetch access token");
      // no point hammering Spotify with credentials it already refused
      BELL_SLEEP_MS(cspot_credentials_rejected ? 10000 : 3000);
    }

    retryCount--;
  } while (retryCount >= 0 && !success);

  keyPending = false;
}
