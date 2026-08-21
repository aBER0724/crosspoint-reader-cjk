#include "ActivityManager.h"

#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <FontManager.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>

#include "CrossPointState.h"
#include "OpdsServerStore.h"
#include "OrientationHelper.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "reader/EpubReaderUtils.h"
#include "reader/ReaderActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FullScreenMessageActivity.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

namespace {
bool hasHeldInput(const MappedInputManager& input) {
  constexpr MappedInputManager::Button buttons[] = {
      MappedInputManager::Button::Back,        MappedInputManager::Button::Confirm,
      MappedInputManager::Button::Left,        MappedInputManager::Button::Right,
      MappedInputManager::Button::Up,          MappedInputManager::Button::Down,
      MappedInputManager::Button::Power,       MappedInputManager::Button::PageBack,
      MappedInputManager::Button::PageForward, MappedInputManager::Button::NavNext,
      MappedInputManager::Button::NavPrevious,
  };
  for (const auto button : buttons) {
    if (input.isPressed(button)) {
      return true;
    }
  }

  float touchX;
  float touchY;
  return gpio.isTouchHeldAt(touchX, touchY);
}
}  // namespace

void ActivityManager::begin() {
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          8192,               // Stack size
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uint32_t renderSerial = renderRequestSerial.load(std::memory_order_relaxed);

    TaskHandle_t waiter = nullptr;
    uint32_t waiterSerial = 0;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waiterSerial = waitingRenderSerial;
    taskEXIT_CRITICAL(&activityManagerSpinlock);

    const bool mustWaitForDisplay = waiter != nullptr && renderSerial >= waiterSerial;
    if (mustWaitForDisplay) {
      // requestUpdateAndWait() promises a completed paint. Drain an older
      // waveform before rendering, and the new one after rendering, without
      // holding RenderLock during either hardware wait.
      renderer.waitRefreshComplete();
    }

    {
      // Acquire the lock before reading currentActivity to avoid a TOCTOU race
      // where the main task deletes the activity between the null-check and render().
      RenderLock renderLock(true, true);

      renderer.setDeferRefreshWhileBusy(!mustWaitForDisplay);
      if (currentActivity) {
        HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
        currentActivity->render(std::move(renderLock));
      }
      renderer.setDeferRefreshWhileBusy(false);
    }

    if (mustWaitForDisplay) {
      renderer.waitRefreshComplete();
    }

    // Notify a waiter only after the particular render it requested has
    // completed. A render already in progress when the wait began must not
    // satisfy the request prematurely.
    TaskHandle_t waiterToNotify = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    if (waitingTaskHandle != nullptr && renderSerial >= waitingRenderSerial) {
      waiterToNotify = waitingTaskHandle;
      waitingTaskHandle = nullptr;
      waitingRenderSerial = 0;
    }
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiterToNotify) {
      xTaskNotify(waiterToNotify, 1, eIncrement);
    }

    completedRenderSerial.store(renderSerial, std::memory_order_release);
  }
}

void ActivityManager::updateReaderUiGlyphCacheMode() {
  const bool readerFontMemoryNeeded = currentActivity && currentActivity->needsReaderFontMemory();
  LOG_DBG("ACT", "Reader font memory %s before UI cache update: free=%u max=%u",
          readerFontMemoryNeeded ? "needed" : "available", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  FontManager::getInstance().setUiGlyphCacheSuspended(readerFontMemoryNeeded);
  LOG_DBG("ACT", "UI cache update complete: free=%u max=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void ActivityManager::loop() {
  const unsigned long now = millis();
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased() || gpio.wasTouchActivity() ||
      hasHeldInput(mappedInput)) {
    lastUserInputMs = now;
  }

  if (currentActivity) {
    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {
      // Home is handled before Activity::loop(), so reader-local input
      // cancellation has not run yet. Let the current render stop before the
      // activity transition takes the mutex.
      cancelCurrentRender();
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      goHome();
      return;
    }

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        updateReaderUiGlyphCacheMode();
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());

        // Apply screen+input orientation for the activity that's coming back
        // to the foreground. Each activity decides via supportsLandscape()
        // whether it wants the configured Landscape orientation or stays in
        // Portrait/Inverted, so popping from a Portrait UI subactivity back
        // to a Landscape reader correctly switches the rotation here.
        OrientationHelper::applyOrientation(renderer, mappedInput, currentActivity.get());

        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);
      updateReaderUiGlyphCacheMode();

      lock.unlock();  // onEnter may acquire its own lock
      // Apply screen+input orientation BEFORE onEnter so the activity's
      // onEnter / first render see the correct orientation. Each activity
      // decides via supportsLandscape() whether to use the configured
      // Landscape orientation or stay Portrait/Inverted; UI subactivities
      // pushed from a Landscape reader therefore switch to Portrait, and
      // popping back rotates the reader to its configured orientation.
      OrientationHelper::applyOrientation(renderer, mappedInput, currentActivity.get());
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate.exchange(false)) {
    notifyRenderTask();
  }

  // A render completed while an older async waveform was active leaves its
  // newest framebuffer staged in GfxRenderer. Submit it only once no newer
  // render is queued or running, otherwise a just-superseded screen could win
  // the race to the panel.
  const uint32_t latestRenderSerial = renderRequestSerial.load(std::memory_order_acquire);
  const uint32_t completedRenderSerial = this->completedRenderSerial.load(std::memory_order_acquire);
  if (!requestedUpdate.load(std::memory_order_relaxed) && completedRenderSerial == latestRenderSerial) {
    RenderLock lock(false);
    if (lock.locked() && !renderer.refreshBusy()) {
      renderer.flushDeferredRefresh();
    }
  }

  flushDeferredPersistence();
}

void ActivityManager::flushDeferredPersistence() {
  if (!appStateSavePending && !EpubReaderUtils::hasQueuedProgressSave() && !Section::hasDeferredCleanup()) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastUserInputMs < DEFERRED_PERSIST_IDLE_MS ||
      now - lastDeferredPersistenceAttemptMs < DEFERRED_PERSIST_RETRY_MS || pendingAction != PendingAction::None ||
      pendingActivity || requestedUpdate.load(std::memory_order_relaxed) || RenderLock::peek() ||
      renderer.refreshBusy()) {
    return;
  }

  const uint32_t latestRenderSerial = renderRequestSerial.load(std::memory_order_acquire);
  const uint32_t completedRenderSerial = this->completedRenderSerial.load(std::memory_order_acquire);
  if (completedRenderSerial != latestRenderSerial) {
    return;
  }

  // Write at most one file per idle window. A slow or fragmented SD card then
  // cannot turn a single deferred maintenance pass into several seconds of I/O.
  lastDeferredPersistenceAttemptMs = now;
  if (EpubReaderUtils::hasQueuedProgressSave()) {
    EpubReaderUtils::flushQueuedProgressSave();
    return;
  }

  if (Section::hasDeferredCleanup()) {
    RenderLock lock(false);
    if (lock.locked()) {
      Section::flushDeferredCleanup();
    }
    return;
  }

  if (APP_STATE.saveToFile()) {
    appStateSavePending = false;
  }
}

void ActivityManager::flushDeferredPersistenceBeforeSleep() {
  if (EpubReaderUtils::hasQueuedProgressSave()) {
    EpubReaderUtils::flushQueuedProgressSave();
  }
  if (appStateSavePending && APP_STATE.saveToFile()) {
    appStateSavePending = false;
  }
}

void ActivityManager::notifyRenderTask() {
  if (!renderTaskHandle) {
    return;
  }
  // Increment counter so multiple rapid calls won't be lost.
  xTaskNotify(renderTaskHandle, 1, eIncrement);
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  invalidateRender();
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    updateReaderUiGlyphCacheMode();
    // Apply orientation before onEnter so the first render is rotated correctly.
    OrientationHelper::applyOrientation(renderer, mappedInput, currentActivity.get());
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToSettings() { replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput)); }

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToRecentBooks() {
  replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToReader(std::string path) {
  LOG_DBG("ACT", "goToReader requested: %s", path.c_str());
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::goHome(HomeMenuItem initialMenuItem) {
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "RecentBooks") {
      initialMenuItem = HomeMenuItem::RECENTS;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem));
}

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  invalidateRender();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  invalidateRender();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::isReaderActivity() const { return currentActivity && currentActivity->isReaderActivity(); }

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

void ActivityManager::requestUpdate(bool immediate) {
  invalidateRender();
  renderRequestSerial.fetch_add(1, std::memory_order_relaxed);
  if (immediate) {
    notifyRenderTask();
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
  invalidateRender();
  const uint32_t waitRenderSerial = renderRequestSerial.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
    waitingRenderSerial = waitRenderSerial;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  notifyRenderTask();
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// RenderLock

RenderLock::RenderLock(const bool wait, const bool trackRenderGeneration)
    : tracksRenderGeneration(trackRenderGeneration) {
  isLocked = xSemaphoreTake(activityManager.renderingMutex, wait ? portMAX_DELAY : 0) == pdTRUE;
  if (isLocked && tracksRenderGeneration) {
    renderGeneration = activityManager.renderGeneration.load(std::memory_order_relaxed);
  }
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

bool RenderLock::isStale() const {
  return tracksRenderGeneration && renderGeneration != activityManager.renderGeneration.load(std::memory_order_relaxed);
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
