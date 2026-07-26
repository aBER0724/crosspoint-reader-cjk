#pragma once

#include <WebServer.h>

#include <string>

namespace WebAdminAuth {

bool isAuthorized(WebServer& server, const std::string& adminToken);
void sendUnauthorized(WebServer& server);

}  // namespace WebAdminAuth
