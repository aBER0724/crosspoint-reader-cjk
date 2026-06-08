#include <Arduino.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>

#include <cstring>

#include "network/FirmwareInstaller.h"

namespace {

constexpr const char* MODULE = "RECOVERY";
constexpr uint32_t RESTART_DELAY_MS = 3000;
constexpr uint32_t FAILURE_REFRESH_DELAY_MS = 30000;
constexpr int CHAR_WIDTH = 5;
constexpr int CHAR_HEIGHT = 7;
constexpr int CHAR_SPACING = 1;
constexpr int TEXT_SCALE = 2;

constexpr const char* SCAN_DIRS[] = {
    "/",
    "/recovery-payload",
};
constexpr size_t MAX_PAYLOAD_PATH_LEN = 96;

struct RecoveryStatus {
  char line1[48];
  char line2[64];
  char line3[64];
  int progressPercent;
};

RecoveryStatus status = {{0}, {0}, {0}, -1};
FirmwareInstaller installer;

// 5x7 ASCII font for recovery diagnostics. Each row uses the low 5 bits.
// Supported glyphs cover the uppercase messages, digits, spaces, and path punctuation used here.
constexpr uint8_t GLYPH_SPACE[CHAR_HEIGHT] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t GLYPH_A[CHAR_HEIGHT] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
constexpr uint8_t GLYPH_B[CHAR_HEIGHT] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
constexpr uint8_t GLYPH_C[CHAR_HEIGHT] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
constexpr uint8_t GLYPH_D[CHAR_HEIGHT] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
constexpr uint8_t GLYPH_E[CHAR_HEIGHT] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
constexpr uint8_t GLYPH_F[CHAR_HEIGHT] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
constexpr uint8_t GLYPH_G[CHAR_HEIGHT] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
constexpr uint8_t GLYPH_H[CHAR_HEIGHT] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
constexpr uint8_t GLYPH_I[CHAR_HEIGHT] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
constexpr uint8_t GLYPH_J[CHAR_HEIGHT] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
constexpr uint8_t GLYPH_K[CHAR_HEIGHT] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
constexpr uint8_t GLYPH_L[CHAR_HEIGHT] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
constexpr uint8_t GLYPH_M[CHAR_HEIGHT] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
constexpr uint8_t GLYPH_N[CHAR_HEIGHT] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
constexpr uint8_t GLYPH_O[CHAR_HEIGHT] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
constexpr uint8_t GLYPH_P[CHAR_HEIGHT] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
constexpr uint8_t GLYPH_Q[CHAR_HEIGHT] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
constexpr uint8_t GLYPH_R[CHAR_HEIGHT] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
constexpr uint8_t GLYPH_S[CHAR_HEIGHT] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
constexpr uint8_t GLYPH_T[CHAR_HEIGHT] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
constexpr uint8_t GLYPH_U[CHAR_HEIGHT] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
constexpr uint8_t GLYPH_V[CHAR_HEIGHT] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
constexpr uint8_t GLYPH_W[CHAR_HEIGHT] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
constexpr uint8_t GLYPH_X[CHAR_HEIGHT] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
constexpr uint8_t GLYPH_Y[CHAR_HEIGHT] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
constexpr uint8_t GLYPH_Z[CHAR_HEIGHT] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
constexpr uint8_t GLYPH_0[CHAR_HEIGHT] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
constexpr uint8_t GLYPH_1[CHAR_HEIGHT] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
constexpr uint8_t GLYPH_2[CHAR_HEIGHT] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
constexpr uint8_t GLYPH_3[CHAR_HEIGHT] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
constexpr uint8_t GLYPH_4[CHAR_HEIGHT] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
constexpr uint8_t GLYPH_5[CHAR_HEIGHT] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
constexpr uint8_t GLYPH_6[CHAR_HEIGHT] = {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
constexpr uint8_t GLYPH_7[CHAR_HEIGHT] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
constexpr uint8_t GLYPH_8[CHAR_HEIGHT] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
constexpr uint8_t GLYPH_9[CHAR_HEIGHT] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};
constexpr uint8_t GLYPH_DASH[CHAR_HEIGHT] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
constexpr uint8_t GLYPH_SLASH[CHAR_HEIGHT] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
constexpr uint8_t GLYPH_DOT[CHAR_HEIGHT] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
constexpr uint8_t GLYPH_COLON[CHAR_HEIGHT] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
constexpr uint8_t GLYPH_PERCENT[CHAR_HEIGHT] = {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03};
constexpr uint8_t GLYPH_UNDERSCORE[CHAR_HEIGHT] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};

const uint8_t* glyphFor(char c) {
  if (c >= 'a' && c <= 'z') {
    c = static_cast<char>(c - 'a' + 'A');
  }

  switch (c) {
    case 'A':
      return GLYPH_A;
    case 'B':
      return GLYPH_B;
    case 'C':
      return GLYPH_C;
    case 'D':
      return GLYPH_D;
    case 'E':
      return GLYPH_E;
    case 'F':
      return GLYPH_F;
    case 'G':
      return GLYPH_G;
    case 'H':
      return GLYPH_H;
    case 'I':
      return GLYPH_I;
    case 'J':
      return GLYPH_J;
    case 'K':
      return GLYPH_K;
    case 'L':
      return GLYPH_L;
    case 'M':
      return GLYPH_M;
    case 'N':
      return GLYPH_N;
    case 'O':
      return GLYPH_O;
    case 'P':
      return GLYPH_P;
    case 'Q':
      return GLYPH_Q;
    case 'R':
      return GLYPH_R;
    case 'S':
      return GLYPH_S;
    case 'T':
      return GLYPH_T;
    case 'U':
      return GLYPH_U;
    case 'V':
      return GLYPH_V;
    case 'W':
      return GLYPH_W;
    case 'X':
      return GLYPH_X;
    case 'Y':
      return GLYPH_Y;
    case 'Z':
      return GLYPH_Z;
    case '0':
      return GLYPH_0;
    case '1':
      return GLYPH_1;
    case '2':
      return GLYPH_2;
    case '3':
      return GLYPH_3;
    case '4':
      return GLYPH_4;
    case '5':
      return GLYPH_5;
    case '6':
      return GLYPH_6;
    case '7':
      return GLYPH_7;
    case '8':
      return GLYPH_8;
    case '9':
      return GLYPH_9;
    case '-':
      return GLYPH_DASH;
    case '/':
      return GLYPH_SLASH;
    case '.':
      return GLYPH_DOT;
    case ':':
      return GLYPH_COLON;
    case '%':
      return GLYPH_PERCENT;
    case '_':
      return GLYPH_UNDERSCORE;
    case ' ':
      return GLYPH_SPACE;
    default:
      return GLYPH_DASH;
  }
}

void setStatus(const char* line1, const char* line2 = "", const char* line3 = "", int progressPercent = -1) {
  snprintf(status.line1, sizeof(status.line1), "%s", line1 ? line1 : "");
  snprintf(status.line2, sizeof(status.line2), "%s", line2 ? line2 : "");
  snprintf(status.line3, sizeof(status.line3), "%s", line3 ? line3 : "");
  status.progressPercent = progressPercent;

  LOG_INF(MODULE, "%s | %s | %s | %d%%", status.line1, status.line2, status.line3, status.progressPercent);
}

void drawPixel(uint8_t* fb, int x, int y) {
  const int logicalWidth = display.getDisplayHeight();
  const int logicalHeight = display.getDisplayWidth();
  if (x < 0 || y < 0 || x >= logicalWidth || y >= logicalHeight) {
    return;
  }

  // Match GfxRenderer::Portrait: logical portrait (480x800) -> physical panel (800x480).
  const int phyX = y;
  const int phyY = display.getDisplayHeight() - 1 - x;
  const size_t index = static_cast<size_t>(phyY) * display.getDisplayWidthBytes() + static_cast<size_t>(phyX / 8);
  fb[index] &= static_cast<uint8_t>(~(0x80 >> (phyX % 8)));
}

void drawChar(uint8_t* fb, int x, int y, char c) {
  const uint8_t* glyph = glyphFor(c);
  for (int row = 0; row < CHAR_HEIGHT; row++) {
    const uint8_t bits = glyph[row];
    for (int col = 0; col < CHAR_WIDTH; col++) {
      if (((bits >> (CHAR_WIDTH - 1 - col)) & 0x01) == 0) {
        continue;
      }
      for (int sy = 0; sy < TEXT_SCALE; sy++) {
        for (int sx = 0; sx < TEXT_SCALE; sx++) {
          drawPixel(fb, x + col * TEXT_SCALE + sx, y + row * TEXT_SCALE + sy);
        }
      }
    }
  }
}

void drawLine(uint8_t* fb, int x, int y, const char* text) {
  if (!text) {
    return;
  }

  const int advance = (CHAR_WIDTH + CHAR_SPACING) * TEXT_SCALE;
  for (int i = 0; i < 58 && text[i] != '\0'; i++) {
    drawChar(fb, x + i * advance, y, text[i]);
  }
}

void renderStatus(HalDisplay::RefreshMode refreshMode = HalDisplay::RefreshMode::FULL_REFRESH) {
  uint8_t* fb = display.getFrameBuffer();
  if (!fb) {
    LOG_ERR(MODULE, "No framebuffer");
    return;
  }

  memset(fb, 0xFF, display.getBufferSize());

  drawLine(fb, 20, 30, status.line1);
  drawLine(fb, 20, 58, status.line2);
  drawLine(fb, 20, 86, status.line3);

  if (status.progressPercent >= 0) {
    constexpr int barX = 20;
    constexpr int barY = 125;
    constexpr int barW = 320;
    constexpr int barH = 20;
    const int fillW = (barW * status.progressPercent) / 100;
    for (int y = barY; y < barY + barH; y++) {
      for (int x = barX; x < barX + barW; x++) {
        const bool border = y == barY || y == barY + barH - 1 || x == barX || x == barX + barW - 1;
        const bool fill = x < barX + fillW;
        if (border || fill) {
          drawPixel(fb, x, y);
        }
      }
    }
  }

  display.displayBuffer(refreshMode, false);
}

bool endsWithBin(const char* name) {
  const size_t len = strlen(name);
  return len > 4 && strcmp(name + len - 4, ".bin") == 0;
}

bool isFirmwareTargetName(const char* name) {
  return name && (strncmp(name, "firmware-", 9) == 0 || strncmp(name, "firmware_", 9) == 0) && endsWithBin(name);
}

bool buildPayloadPath(const char* dir, const char* name, char* out, size_t outLen) {
  if (!dir || !name || !out || outLen == 0) {
    return false;
  }

  const int written =
      (strcmp(dir, "/") == 0) ? snprintf(out, outLen, "/%s", name) : snprintf(out, outLen, "%s/%s", dir, name);
  return written > 0 && static_cast<size_t>(written) < outLen;
}

bool findPayload(char* out, size_t outLen) {
  if (!out || outLen == 0) {
    return false;
  }
  out[0] = '\0';

  for (const char* dirPath : SCAN_DIRS) {
    HalFile dir;
    if (!Storage.openFileForRead(MODULE, dirPath, dir) || !dir.isDirectory()) {
      dir.close();
      continue;
    }

    while (true) {
      HalFile entry = dir.openNextFile();
      if (!entry) {
        break;
      }

      char name[64];
      name[0] = '\0';
      entry.getName(name, sizeof(name));
      const bool isCandidate =
          !entry.isDirectory() && isFirmwareTargetName(name) && buildPayloadPath(dirPath, name, out, outLen);
      entry.close();

      if (isCandidate) {
        dir.close();
        return true;
      }
    }

    dir.close();
  }

  return false;
}

void progressCallback(size_t processed, size_t total, void*) {
  static int lastPercent = -1;
  if (total == 0) {
    return;
  }

  const int percent = static_cast<int>((processed * 100U) / total);
  if (percent == lastPercent || (percent - lastPercent < 5 && percent != 100)) {
    return;
  }

  lastPercent = percent;
  char progressLine[64];
  snprintf(progressLine, sizeof(progressLine), "%u/%u BYTES", static_cast<unsigned>(processed),
           static_cast<unsigned>(total));
  setStatus("INSTALLING PAYLOAD", progressLine, "DO NOT POWER OFF", percent);
  renderStatus(HalDisplay::RefreshMode::FAST_REFRESH);
}

[[noreturn]] void failAndWait(const char* code, const char* detail) {
  setStatus("RECOVERY FAILED", code, detail ? detail : "CHECK SD CARD FILES");
  renderStatus();
  while (true) {
    delay(FAILURE_REFRESH_DELAY_MS);
  }
}

void installPayload(const char* path) {
  char foundLine[64];
  snprintf(foundLine, sizeof(foundLine), "FOUND %s", path);
  setStatus("RECOVERY MODE", foundLine, "OPENING PAYLOAD");
  renderStatus();

  HalFile file;
  if (!Storage.openFileForRead(MODULE, path, file)) {
    failAndWait("OPEN FAILED", path);
  }

  const size_t size = file.fileSize();
  const FirmwareInstaller::Error result = installer.installFromFile(file, size, progressCallback, nullptr);
  file.close();

  if (result != FirmwareInstaller::Error::OK) {
    char code[32];
    snprintf(code, sizeof(code), "INSTALL %u", static_cast<unsigned>(result));
    failAndWait(code, path);
  }

  setStatus("INSTALL COMPLETE", "RESTARTING DEVICE", "REMOVE RECOVERY FILES", 100);
  renderStatus();
  delay(RESTART_DELAY_MS);
  ESP.restart();
}

void runRecovery() {
  HalSystem::begin();
  gpio.begin();
  powerManager.begin();

  logSerial.begin(115200);
  delay(500);
  LOG_INF(MODULE, "Starting SD recovery firmware");

  display.begin();
  setStatus("RECOVERY MODE", "CHECKING SD CARD", "");
  renderStatus();

  if (!Storage.begin() || !Storage.ready()) {
    failAndWait("SD NOT READY", "REINSERT SD CARD");
  }

  char payload[MAX_PAYLOAD_PATH_LEN];
  if (!findPayload(payload, sizeof(payload))) {
    failAndWait("PAYLOAD NOT FOUND", "USE FIRMWARE-TARGET BIN");
  }

  installPayload(payload);
}

}  // namespace

void setup() { runRecovery(); }

void loop() { delay(1000); }
