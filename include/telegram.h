#ifndef TELEGRAM_H
#define TELEGRAM_H

#include <Arduino.h>

// Initialize Telegram queue and state
void telegramInit();

// FreeRTOS task entry point
void telegramTask(void* param);

// Non-blocking send functions (enqueue to task)
void telegramSendText(const char* msg);
void telegramSendPhoto(const uint8_t* jpeg, size_t len, const char* caption);

// Check if Telegram is configured (token + chat_id set)
bool isTelegramConnected();

// Active hours helper
bool isWithinActiveHours();

#endif // TELEGRAM_H
