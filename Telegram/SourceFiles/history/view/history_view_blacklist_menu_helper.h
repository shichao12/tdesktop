/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/basic_click_handlers.h"
#include "ui/text/text_entity.h"

namespace HistoryView {

[[nodiscard]] inline QString BlacklistKeywordFromSelection(
		const TextForMimeData &selection) {
	return selection.expanded.simplified();
}

[[nodiscard]] inline QString BlacklistLinkFromHandler(
		const ClickHandlerPtr &link) {
	if (!link) {
		return QString();
	}
	const auto entity = link->getTextEntity();
	switch (entity.type) {
	case EntityType::Url:
		return entity.data.trimmed();
	case EntityType::CustomUrl: {
		auto result = UrlClickHandler::ExternalUrlFromInternalUrl(
			entity.data);
		if (result.isEmpty()) {
			result = entity.data;
		}
		return result.startsWith(u"internal:"_q, Qt::CaseInsensitive)
			? QString()
			: result.trimmed();
	}
	default:
		return QString();
	}
}

} // namespace HistoryView
