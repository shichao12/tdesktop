/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_message_keyword_blacklist.h"

#include "base/weak_ptr.h"
#include "base/flat_map.h"
#include "data/data_message_keyword_blacklist.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace Window {
namespace {

constexpr auto kMaxMessageBlacklistKeywordLength = 128;

[[nodiscard]] std::vector<QString> CurrentKeywords(
		not_null<SessionController*> controller,
		PeerData *peer) {
	auto &blacklist = controller->session().data().messageKeywordBlacklist();
	const auto &keywords = peer
		? blacklist.channelKeywords(peer->id)
		: blacklist.commonKeywords();
	return { begin(keywords), end(keywords) };
}

void SaveKeywords(
		not_null<SessionController*> controller,
		PeerData *peer,
		std::vector<QString> keywords) {
	auto &blacklist = controller->session().data().messageKeywordBlacklist();
	if (peer) {
		blacklist.setChannelKeywords(peer->id, std::move(keywords));
	} else {
		blacklist.setCommonKeywords(std::move(keywords));
	}
}

[[nodiscard]] QString ItemPreview(not_null<HistoryItem*> item) {
	const auto text = item->originalText().text.simplified();
	return text.isEmpty()
		? tr::lng_message_blacklist_open(tr::now)
		: text;
}

void OpenItem(
		not_null<SessionController*> controller,
		FullMsgId itemId,
		base::weak_qptr<Ui::GenericBox> box) {
	const auto item = controller->session().data().message(itemId);
	if (!item) {
		if (const auto strong = box.get()) {
			strong->closeBox();
		}
		return;
	}
	auto &blacklist = item->history()->owner().messageKeywordBlacklist();
	blacklist.setExpanded(item, true);
	if (const auto strong = box.get()) {
		strong->closeBox();
	}
	const auto weak = base::make_weak(controller);
	crl::on_main(weak, [=] {
		if (const auto item = controller->session().data().message(itemId)) {
			controller->showMessage(item, SectionShow::Way::Forward);
		}
	});
}

void ShowAddKeywordBox(
		base::weak_qptr<Ui::GenericBox> parent,
		not_null<SessionController*> controller,
		PeerData *peer) {
	const auto strong = parent.get();
	if (!strong) {
		return;
	}
	strong->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_message_blacklist_add_keyword());
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			tr::lng_message_blacklist_keyword_placeholder(),
			QString()));
		field->setMaxLength(kMaxMessageBlacklistKeywordLength);
		box->setFocusCallback([=] {
			field->setFocusFast();
		});
		const auto submit = [=] {
			const auto keyword = field->getLastText().trimmed();
			if (keyword.isEmpty()) {
				return;
			}
			auto keywords = CurrentKeywords(controller, peer);
			keywords.push_back(keyword);
			SaveKeywords(controller, peer, std::move(keywords));
			box->closeBox();
		};
		field->submits() | rpl::on_next(submit, field->lifetime());
		box->addButton(tr::lng_message_blacklist_add_keyword(), submit);
		box->addButton(
			tr::lng_message_blacklist_cancel(),
			[=] { box->closeBox(); });
	}));
}

void FillKeywords(
		base::weak_qptr<Ui::GenericBox> box,
		not_null<Ui::VerticalLayout*> rows,
		not_null<SessionController*> controller,
		PeerData *peer) {
	if (!box) {
		return;
	}
	rows->clear();

	Settings::AddButtonWithIcon(
		rows,
		tr::lng_message_blacklist_add_keyword(),
		st::settingsButtonActive,
		{ &st::menuIconAdd }
	)->addClickHandler([=] {
		ShowAddKeywordBox(box, controller, peer);
	});

	const auto keywords = CurrentKeywords(controller, peer);
	if (keywords.empty()) {
		Ui::AddDividerText(rows, tr::lng_message_blacklist_no_keywords());
		return;
	}

	for (const auto &keyword : keywords) {
		Settings::AddButtonWithLabel(
			rows,
			rpl::single(keyword),
			tr::lng_message_blacklist_delete_keyword(),
			st::settingsButton,
			{ &st::menuIconDelete }
		)->addClickHandler([=] {
			auto keywords = CurrentKeywords(controller, peer);
			for (auto i = begin(keywords); i != end(keywords); ++i) {
				if (*i == keyword) {
					keywords.erase(i);
					break;
				}
			}
			SaveKeywords(controller, peer, std::move(keywords));
		});
	}
}

void FillItems(
		base::weak_qptr<Ui::GenericBox> box,
		not_null<Ui::VerticalLayout*> rows,
		not_null<SessionController*> controller,
		PeerData *peer) {
	if (!box) {
		return;
	}
	rows->clear();

	auto &blacklist = controller->session().data().messageKeywordBlacklist();
	const auto items = blacklist.matchedItems(peer);
	if (items.empty()) {
		Ui::AddDividerText(rows, tr::lng_message_blacklist_empty());
		return;
	}

	struct Group {
		not_null<PeerData*> peer;
		std::vector<FullMsgId> itemIds;
		std::vector<QString> previews;
	};
	auto groups = std::vector<Group>();
	auto groupIndex = base::flat_map<PeerId, int>();
	for (const auto item : items) {
		const auto itemPeer = item->history()->peer;
		const auto index = [&] {
			const auto i = groupIndex.find(itemPeer->id);
			if (i != end(groupIndex)) {
				return i->second;
			}
			const auto result = int(groups.size());
			groupIndex.emplace(itemPeer->id, result);
			groups.push_back({ itemPeer, {}, {} });
			return result;
		}();
		auto &group = groups[index];
		group.itemIds.push_back(item->fullId());
		group.previews.push_back(ItemPreview(item));
	}

	for (const auto &group : groups) {
		if (!peer) {
			Ui::AddSubsectionTitle(rows, rpl::single(group.peer->name()));
		}
		for (auto i = 0, count = int(group.itemIds.size()); i != count; ++i) {
			const auto itemId = group.itemIds[i];
			Settings::AddButtonWithIcon(
				rows,
				rpl::single(group.previews[i]),
				st::settingsButton,
				{ &st::menuIconShowInChat }
			)->addClickHandler([=] {
				OpenItem(controller, itemId, box);
			});
		}
	}
}

} // namespace

void ShowMessageKeywordBlacklistKeywordsBox(
		not_null<SessionController*> controller,
		PeerData *peer) {
	if (peer && !peer->asBroadcast()) {
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(peer
			? tr::lng_message_blacklist_channel_keywords()
			: tr::lng_message_blacklist_common_keywords());
		box->setMaxHeight(st::boxMaxListHeight);
		box->addButton(
			tr::lng_message_blacklist_close(),
			[=] { box->closeBox(); });

		const auto rows = box->verticalLayout()->add(
			object_ptr<Ui::VerticalLayout>(box->verticalLayout()));
		const auto weakBox = base::make_weak(box);
		const auto weakRows = base::make_weak(rows);
		FillKeywords(weakBox, rows, controller, peer);

		controller->session().data().messageKeywordBlacklist().changed(
		) | rpl::on_next([=] {
			if (const auto strongRows = weakRows.get()) {
				FillKeywords(weakBox, strongRows, controller, peer);
			}
		}, box->lifetime());
	}));
}

void ShowMessageKeywordBlacklistItemsBox(
		not_null<SessionController*> controller,
		PeerData *peer) {
	if (peer && !peer->asBroadcast()) {
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(peer
			? tr::lng_message_blacklist_view_channel()
			: tr::lng_message_blacklist_title());
		box->setMaxHeight(st::boxMaxListHeight);
		box->addButton(
			tr::lng_message_blacklist_close(),
			[=] { box->closeBox(); });

		const auto rows = box->verticalLayout()->add(
			object_ptr<Ui::VerticalLayout>(box->verticalLayout()));
		const auto weakBox = base::make_weak(box);
		const auto weakRows = base::make_weak(rows);
		FillItems(weakBox, rows, controller, peer);

		controller->session().data().messageKeywordBlacklist().changed(
		) | rpl::on_next([=] {
			if (const auto strongRows = weakRows.get()) {
				FillItems(weakBox, strongRows, controller, peer);
			}
		}, box->lifetime());
	}));
}

} // namespace Window
