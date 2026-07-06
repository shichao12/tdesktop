/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/basic_click_handlers.h"
#include "ui/text/text_entity.h"

#include <vector>

namespace HistoryView {

[[nodiscard]] inline QString BlacklistKeywordFromSelection(
		const TextForMimeData &selection) {
	return selection.expanded.simplified();
}

[[nodiscard]] inline QString BlacklistTagFromHandler(
		const ClickHandlerPtr &link) {
	if (!link) {
		return QString();
	}
	const auto type = link->getTextEntity().type;
	return (type == EntityType::Hashtag || type == EntityType::Cashtag)
		? link->copyToClipboardText().trimmed()
		: QString();
}

[[nodiscard]] inline QString BlacklistNormalizedTextLink(QString text) {
	return text.trimmed().toCaseFolded();
}

[[nodiscard]] inline bool BlacklistTextLinkInList(
		const std::vector<QString> &texts,
		const QString &text) {
	const auto normalized = BlacklistNormalizedTextLink(text);
	if (normalized.isEmpty()) {
		return false;
	}
	for (const auto &entry : texts) {
		if (BlacklistNormalizedTextLink(entry) == normalized) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline std::vector<QString> BlacklistTextLinksWithout(
		std::vector<QString> texts,
		const QString &text) {
	const auto normalized = BlacklistNormalizedTextLink(text);
	if (normalized.isEmpty()) {
		return texts;
	}
	auto result = std::vector<QString>();
	result.reserve(texts.size());
	for (auto &entry : texts) {
		if (BlacklistNormalizedTextLink(entry) != normalized) {
			result.push_back(std::move(entry));
		}
	}
	return result;
}

[[nodiscard]] inline QString BlacklistNormalizedLink(QString link) {
	link = link.trimmed().toCaseFolded();
	while (link.endsWith('/')) {
		link.chop(1);
	}
	return link;
}

[[nodiscard]] inline bool BlacklistLinkInList(
		const std::vector<QString> &links,
		const QString &link) {
	const auto normalized = BlacklistNormalizedLink(link);
	if (normalized.isEmpty()) {
		return false;
	}
	for (const auto &entry : links) {
		if (BlacklistNormalizedLink(entry) == normalized) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline std::vector<QString> BlacklistLinksWithout(
		std::vector<QString> links,
		const QString &link) {
	const auto normalized = BlacklistNormalizedLink(link);
	if (normalized.isEmpty()) {
		return links;
	}
	auto result = std::vector<QString>();
	result.reserve(links.size());
	for (auto &entry : links) {
		if (BlacklistNormalizedLink(entry) != normalized) {
			result.push_back(std::move(entry));
		}
	}
	return result;
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
