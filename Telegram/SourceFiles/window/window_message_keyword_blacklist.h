/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;

namespace Window {

class SessionController;

void ShowMessageKeywordBlacklistKeywordsBox(
	not_null<SessionController*> controller,
	PeerData *peer);

void ShowMessageKeywordBlacklistItemsBox(
	not_null<SessionController*> controller,
	PeerData *peer = nullptr);

} // namespace Window
