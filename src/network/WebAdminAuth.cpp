#include "WebAdminAuth.h"

namespace WebAdminAuth {

bool isAuthorized(WebServer& server, const std::string& adminToken) {
  if (adminToken.empty()) {
    return false;
  }
  const String headerToken = server.header("X-CrossPoint-Token");
  if (!headerToken.isEmpty() && headerToken == adminToken.c_str()) {
    return true;
  }
  return server.hasArg("token") && server.arg("token") == adminToken.c_str();
}

void sendUnauthorized(WebServer& server) { server.send(401, "text/plain", "Unauthorized"); }

}  // namespace WebAdminAuth
