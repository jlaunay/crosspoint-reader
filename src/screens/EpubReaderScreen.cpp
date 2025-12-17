#include "EpubReaderScreen.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <SD.h>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "EpubReaderChapterSelectionScreen.h"
#include "EpubReaderFootnotesScreen.h"
#include "EpubReaderMenuScreen.h"
#include "config.h"

constexpr int PAGES_PER_REFRESH = 15;
constexpr unsigned long SKIP_CHAPTER_MS = 700;
constexpr float lineCompression = 0.95f;
constexpr int marginTop = 8;
constexpr int marginRight = 10;
constexpr int marginBottom = 22;
constexpr int marginLeft = 10;

void EpubReaderScreen::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderScreen*>(param);
  self->displayTaskLoop();
}

void EpubReaderScreen::onEnter() {
  if (!epub) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();
  epub->setupCacheDir();

  if (SD.exists((epub->getCachePath() + "/progress.bin").c_str())) {
    File f = SD.open((epub->getCachePath() + "/progress.bin").c_str());
    uint8_t data[4];
    f.read(data, 4);
    currentSpineIndex = data[0] + (data[1] << 8);
    nextPageNumber = data[2] + (data[3] << 8);
    Serial.printf("[%lu] [ERS] Loaded cache: %d, %d\n", millis(), currentSpineIndex, nextPageNumber);
    f.close();
  }

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&EpubReaderScreen::taskTrampoline, "EpubReaderScreenTask",
              24576,  // 32768
              this, 1, &displayTaskHandle);
}

void EpubReaderScreen::onExit() {
  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  section.reset();
  epub.reset();
}

void EpubReaderScreen::handleInput() {
  // Pass input responsibility to sub screen if exists
  if (subScreen) {
    subScreen->handleInput();
    return;
  }

  // Enter Menu selection screen
  if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    if (isViewingFootnote) {
      restoreSavedPosition();
      updateRequired = true;
      return;
    } else {
      onGoBack();
      return;
    }
  }
  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    // Don't start screen transition while rendering
    xSemaphoreTake(renderingMutex, portMAX_DELAY);

    subScreen.reset(new EpubReaderMenuScreen(
        this->renderer, this->inputManager,
        [this] {
          // onGoBack - return to reading
          subScreen->onExit();
          subScreen.reset();
          updateRequired = true;
        },
        [this](EpubReaderMenuScreen::MenuOption option) {
          // onSelectOption - handle menu choice
          if (option == EpubReaderMenuScreen::CHAPTERS) {
            // Show chapter selection
            subScreen->onExit();
            subScreen.reset(new EpubReaderChapterSelectionScreen(
                this->renderer, this->inputManager, epub, currentSpineIndex,
                [this] {
                  // onGoBack from chapter selection
                  subScreen->onExit();
                  subScreen.reset();
                  updateRequired = true;
                },
                [this](const int newSpineIndex) {
                  // onSelectSpineIndex
                  if (currentSpineIndex != newSpineIndex) {
                    currentSpineIndex = newSpineIndex;
                    nextPageNumber = 0;
                    section.reset();
                  }
                  subScreen->onExit();
                  subScreen.reset();
                  updateRequired = true;
                }));
            subScreen->onEnter();
          } else if (option == EpubReaderMenuScreen::FOOTNOTES) {
            // Show footnotes page with current page notes
            subScreen->onExit();

            subScreen.reset(new EpubReaderFootnotesScreen(
                this->renderer, this->inputManager,
                currentPageFootnotes,  // Pass collected footnotes (reference)
                [this] {
                  // onGoBack from footnotes
                  subScreen->onExit();
                  subScreen.reset();
                  updateRequired = true;
                },
                [this](const char* href) {
                  // onSelectFootnote - navigate to the footnote location
                  navigateToHref(href, true);  // true = save current position
                  subScreen->onExit();
                  subScreen.reset();
                  updateRequired = true;
                }));
            subScreen->onEnter();
          }
        }));

    subScreen->onEnter();
    xSemaphoreGive(renderingMutex);
  }

  if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    onGoBack();
    return;
  }

  const bool prevReleased =
      inputManager.wasReleased(InputManager::BTN_UP) || inputManager.wasReleased(InputManager::BTN_LEFT);
  const bool nextReleased =
      inputManager.wasReleased(InputManager::BTN_DOWN) || inputManager.wasReleased(InputManager::BTN_RIGHT);

  if (!prevReleased && !nextReleased) {
    return;
  }

  // any button press when at end of the book goes back to the last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = UINT16_MAX;
    updateRequired = true;
    return;
  }

  const bool skipChapter = inputManager.getHeldTime() > SKIP_CHAPTER_MS;

  if (skipChapter) {
    // We don't want to delete the section mid-render, so grab the semaphore
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    nextPageNumber = 0;
    currentSpineIndex = nextReleased ? currentSpineIndex + 1 : currentSpineIndex - 1;
    section.reset();
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    updateRequired = true;
    return;
  }

  if (prevReleased) {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = UINT16_MAX;
      currentSpineIndex--;
      section.reset();
      xSemaphoreGive(renderingMutex);
    }
    updateRequired = true;
  } else {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = 0;
      currentSpineIndex++;
      section.reset();
      xSemaphoreGive(renderingMutex);
    }
    updateRequired = true;
  }
}

void EpubReaderScreen::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubReaderScreen::renderScreen() {
  if (!epub) {
    return;
  }

  // Edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // Based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(READER_FONT_ID, 300, "End of book", true, BOLD);
    renderer.displayBuffer();
    return;
  }

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex);
    Serial.printf("[%lu] [ERS] Loading file: %s, index: %d\n", millis(), filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    if (!section->loadCacheMetadata(READER_FONT_ID, lineCompression, marginTop, marginRight, marginBottom, marginLeft,
                                    SETTINGS.extraParagraphSpacing)) {
      Serial.printf("[%lu] [ERS] Cache not found, building...\n", millis());

      {
        renderer.grayscaleRevert();

        const int textWidth = renderer.getTextWidth(READER_FONT_ID, "Indexing...");
        constexpr int margin = 20;
        // Round all coordinates to 8 pixel boundaries
        const int x = ((GfxRenderer::getScreenWidth() - textWidth - margin * 2) / 2 + 7) / 8 * 8;
        constexpr int y = 56;
        const int w = (textWidth + margin * 2 + 7) / 8 * 8;
        const int h = (renderer.getLineHeight(READER_FONT_ID) + margin * 2 + 7) / 8 * 8;
        renderer.fillRect(x, y, w, h, false);
        renderer.drawText(READER_FONT_ID, x + margin, y + margin, "Indexing...");
        renderer.drawRect(x + 5, y + 5, w - 10, h - 10);
        // EXPERIMENTAL: Still suffers from ghosting
        renderer.displayWindow(x, y, w, h);
        pagesUntilFullRefresh = 0;
      }

      section->setupCacheDir();
      if (!section->persistPageDataToSD(READER_FONT_ID, lineCompression, marginTop, marginRight, marginBottom,
                                        marginLeft, SETTINGS.extraParagraphSpacing)) {
        Serial.printf("[%lu] [ERS] Failed to persist page data to SD\n", millis());
        section.reset();
        return;
      }
    } else {
      Serial.printf("[%lu] [ERS] Cache found, skipping build...\n", millis());
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    Serial.printf("[%lu] [ERS] No pages to render\n", millis());
    renderer.drawCenteredText(READER_FONT_ID, 300, "Empty chapter", true, BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    Serial.printf("[%lu] [ERS] Page out of bounds: %d (max %d)\n", millis(), section->currentPage, section->pageCount);
    renderer.drawCenteredText(READER_FONT_ID, 300, "Out of bounds", true, BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    return;
  }

  // Load page from SD - use pointer to avoid copying on stack
  std::unique_ptr<Page> p = section->loadPageFromSD();
  if (!p) {
    Serial.printf("[%lu] [ERS] Failed to load page from SD - clearing section cache\n", millis());
    section->clearCache();
    section.reset();
    return renderScreen();
  }

  // Copy footnotes from page to currentPageFootnotes
  currentPageFootnotes.clear();
  for (int i = 0; i < p->footnoteCount && i < 16; i++) {
    FootnoteEntry* footnote = p->getFootnote(i);
    if (footnote) {
      currentPageFootnotes.addFootnote(footnote->number, footnote->href);
    }
  }
  Serial.printf("[%lu] [ERS] Loaded %d footnotes for current page\n", millis(), p->footnoteCount);

  const auto start = millis();
  renderContents(std::move(p));
  Serial.printf("[%lu] [ERS] Rendered page in %dms\n", millis(), millis() - start);

  // Save progress
  File f = SD.open((epub->getCachePath() + "/progress.bin").c_str(), FILE_WRITE);
  if (f) {
    uint8_t data[4];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = section->currentPage & 0xFF;
    data[3] = (section->currentPage >> 8) & 0xFF;
    f.write(data, 4);
    f.close();
  }
}

void EpubReaderScreen::renderContents(std::unique_ptr<Page> page) {
  page->render(renderer, READER_FONT_ID);
  renderStatusBar();
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = PAGES_PER_REFRESH;
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Save bw buffer to reset buffer state after grayscale data sync
  renderer.storeBwBuffer();

  // grayscale rendering
  // TODO: Only do this if font supports it
  {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, READER_FONT_ID);
    renderer.copyGrayscaleLsbBuffers();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, READER_FONT_ID);
    renderer.copyGrayscaleMsbBuffers();

    // display grayscale part
    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  // restore the bw data
  renderer.restoreBwBuffer();
}

void EpubReaderScreen::renderStatusBar() const {
  constexpr auto textY = 776;
  // Right aligned text for progress counter
  char progressBuf[32];  // Use fixed buffer instead of std::string
  snprintf(progressBuf, sizeof(progressBuf), "%d / %d", section->currentPage + 1, section->pageCount);
  const auto progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressBuf);
  renderer.drawText(SMALL_FONT_ID, GfxRenderer::getScreenWidth() - marginRight - progressTextWidth, textY, progressBuf);

  // Left aligned battery icon and percentage
  const uint16_t percentage = battery.readPercentage();
  char percentageBuf[8];  // Use fixed buffer instead of std::string
  snprintf(percentageBuf, sizeof(percentageBuf), "%d%%", percentage);
  const auto percentageTextWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageBuf);
  renderer.drawText(SMALL_FONT_ID, 20 + marginLeft, textY, percentageBuf);

  // Battery icon drawing
  constexpr int batteryWidth = 15;
  constexpr int batteryHeight = 10;
  constexpr int x = marginLeft;
  constexpr int y = 783;

  // Top line
  renderer.drawLine(x, y, x + batteryWidth - 4, y);
  // Bottom line
  renderer.drawLine(x, y + batteryHeight - 1, x + batteryWidth - 4, y + batteryHeight - 1);
  // Left line
  renderer.drawLine(x, y, x, y + batteryHeight - 1);
  // Battery end
  renderer.drawLine(x + batteryWidth - 4, y, x + batteryWidth - 4, y + batteryHeight - 1);
  renderer.drawLine(x + batteryWidth - 3, y + 2, x + batteryWidth - 1, y + 2);
  renderer.drawLine(x + batteryWidth - 3, y + batteryHeight - 3, x + batteryWidth - 1, y + batteryHeight - 3);
  renderer.drawLine(x + batteryWidth - 1, y + 2, x + batteryWidth - 1, y + batteryHeight - 3);

  // Fill battery based on percentage
  int filledWidth = percentage * (batteryWidth - 5) / 100 + 1;
  if (filledWidth > batteryWidth - 5) {
    filledWidth = batteryWidth - 5;
  }
  renderer.fillRect(x + 1, y + 1, filledWidth, batteryHeight - 2);

  // Centered chapter title text
  const int titleMarginLeft = 20 + percentageTextWidth + 30 + marginLeft;
  const int titleMarginRight = progressTextWidth + 30 + marginRight;
  const int availableTextWidth = GfxRenderer::getScreenWidth() - titleMarginLeft - titleMarginRight;
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);

  if (tocIndex == -1) {
    const char* title = "Unnamed";
    const int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title);
    renderer.drawText(SMALL_FONT_ID, titleMarginLeft + (availableTextWidth - titleWidth) / 2, textY, title);
  } else {
    const auto& tocItem = epub->getTocItem(tocIndex);
    std::string title = tocItem.title;
    int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());

    // Truncate title if too long
    while (titleWidth > availableTextWidth && title.length() > 8) {
      title = title.substr(0, title.length() - 8) + "...";
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID, titleMarginLeft + (availableTextWidth - titleWidth) / 2, textY, title.c_str());
  }
}

void EpubReaderScreen::navigateToHref(const char* href, bool savePosition) {
  if (!epub || !href) return;

  // Save current position if requested
  if (savePosition && section) {
    savedSpineIndex = currentSpineIndex;
    savedPageNumber = section->currentPage;
    isViewingFootnote = true;
    Serial.printf("[%lu] [ERS] Saved position: spine %d, page %d\n", millis(), savedSpineIndex, savedPageNumber);
  }

  // Parse href: "filename.html#anchor"
  std::string hrefStr(href);
  std::string filename;
  std::string anchor;

  size_t hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos) {
    filename = hrefStr.substr(0, hashPos);
    anchor = hrefStr.substr(hashPos + 1);
  } else {
    filename = hrefStr;
  }

  // Extract just filename without path
  size_t lastSlash = filename.find_last_of('/');
  if (lastSlash != std::string::npos) {
    filename = filename.substr(lastSlash + 1);
  }

  Serial.printf("[%lu] [ERS] Navigate to: %s (anchor: %s)\n", millis(), filename.c_str(), anchor.c_str());

  int targetSpineIndex = -1;

  // FIRST: Check if we have an inline footnote or paragraph note for this anchor
  if (!anchor.empty()) {
    // Try inline footnote first
    std::string inlineFilename = "inline_" + anchor + ".html";
    Serial.printf("[%lu] [ERS] Looking for inline footnote: %s\n", millis(), inlineFilename.c_str());

    targetSpineIndex = epub->findVirtualSpineIndex(inlineFilename);

    // If not found, try paragraph note
    if (targetSpineIndex == -1) {
      std::string pnoteFilename = "pnote_" + anchor + ".html";
      Serial.printf("[%lu] [ERS] Looking for paragraph note: %s\n", millis(), pnoteFilename.c_str());

      targetSpineIndex = epub->findVirtualSpineIndex(pnoteFilename);
    }

    if (targetSpineIndex != -1) {
      Serial.printf("[%lu] [ERS] Found note at virtual index: %d\n", millis(), targetSpineIndex);

      // Navigate to the note
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      currentSpineIndex = targetSpineIndex;
      nextPageNumber = 0;
      section.reset();
      xSemaphoreGive(renderingMutex);

      updateRequired = true;
      return;
    } else {
      Serial.printf("[%lu] [ERS] No virtual note found, trying normal navigation\n", millis());
    }
  }

  // FALLBACK: Try to find the file in normal spine items
  for (int i = 0; i < epub->getSpineItemsCount(); i++) {
    if (epub->isVirtualSpineItem(i)) continue;

    std::string spineItem = epub->getSpineItem(i);
    size_t lastSlash = spineItem.find_last_of('/');
    std::string spineFilename = (lastSlash != std::string::npos) ? spineItem.substr(lastSlash + 1) : spineItem;

    if (spineFilename == filename) {
      targetSpineIndex = i;
      break;
    }
  }

  if (targetSpineIndex == -1) {
    Serial.printf("[%lu] [ERS] Could not find spine index for: %s\n", millis(), filename.c_str());
    return;
  }

  // Navigate to the target chapter
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  currentSpineIndex = targetSpineIndex;
  nextPageNumber = 0;
  section.reset();
  xSemaphoreGive(renderingMutex);

  updateRequired = true;

  Serial.printf("[%lu] [ERS] Navigated to spine index: %d\n", millis(), targetSpineIndex);
}

// Method to restore saved position
void EpubReaderScreen::restoreSavedPosition() {
  if (savedSpineIndex >= 0 && savedPageNumber >= 0) {
    Serial.printf("[%lu] [ERS] Restoring position: spine %d, page %d\n", millis(), savedSpineIndex, savedPageNumber);

    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    currentSpineIndex = savedSpineIndex;
    nextPageNumber = savedPageNumber;
    section.reset();
    xSemaphoreGive(renderingMutex);

    savedSpineIndex = -1;
    savedPageNumber = -1;
    isViewingFootnote = false;
  }
}
