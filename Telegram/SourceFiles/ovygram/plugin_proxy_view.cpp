#include "ovygram/plugin_proxy_view.h"

#include "base/unique_qptr.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <algorithm>
#include <array>
#include <exception>

namespace Ovygram {
namespace {

const style::RoundButton &SelectorStyle() {
	static const auto value = [] {
		auto result = st::defaultLightButton;
		result.radius = style::ConvertScale(7);
		result.textBg = st::windowBgOver;
		result.textFg = st::windowFg;
		result.textFgOver = st::windowFgOver;
		return result;
	}();
	return value;
}

const style::FlatLabel &RouteLabelStyle() {
	static const auto value = [] {
		auto result = st::defaultFlatLabel;
		result.minWidth = 1;
		return result;
	}();
	return value;
}

} // namespace

QString PluginProxyLabel(const QJsonObject &offer) {
	const auto label = offer.value(u"label"_q).toString();
	return (label.isEmpty() ? offer.value(u"plugin_id"_q).toString() : label)
		+ u" · "_q + offer.value(u"owner"_q).toString().left(8);
}

void SetupPluginProxyBox(
		not_null<Ui::GenericBox*> box,
		int width,
		Fn<ProxyViewState()> query,
		Fn<void(const QString&, const QJsonObject&)> apply) {
	box->setWidth(width);
	box->setTitle(rpl::single(u"Прокси плагинов"_q));
	box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });
	const auto padding = st::boxRowPadding;
	const auto subtitle = box->addRow(object_ptr<Ui::FlatLabel>(box,
		u"Назначение прокси по типам трафика"_q, RouteLabelStyle()),
		QMargins(padding.left(), 0, padding.right(), style::ConvertScale(14)));
	subtitle->setTextColorOverride(st::windowSubTextFg->c);
	auto divider = object_ptr<Ui::RpWidget>(box);
	const auto line = divider.data();
	line->resize(1, st::lineWidth);
	line->paintRequest() | rpl::on_next([=](QRect rect) {
		QPainter(line).fillRect(rect, st::shadowFg);
	}, line->lifetime());
	box->addRow(std::move(divider), QMargins(), style::al_justify);

	struct Control {
		QPointer<Ui::RoundButton> select;
		QPointer<Ui::RoundButton> reset;
		QPointer<Ui::RpWidget> dot;
	};
	struct State {
		std::map<QString, Control> controls;
		ProxyViewState data;
		base::unique_qptr<Ui::PopupMenu> menu;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto errorLabel = box->addRow(object_ptr<Ui::FlatLabel>(box,
		QString(), RouteLabelStyle()));
	errorLabel->setTextColorOverride(st::attentionButtonFg->c);
	const auto refresh = [=] {
		for (const auto &[route, control] : state->controls) {
			const auto active = state->data.active.find(route);
			control.select->setText(rpl::single((active != end(state->data.active))
				? (u"\u00A0\u00A0 "_q + active->second + u" (подключён)  ⌄"_q)
				: (state->data.saved.contains(route) ? u"Ожидает запуска  ⌄"_q : u"По умолчанию  ⌄"_q)));
			control.select->setToolTip(control.select->accessibilityName());
			control.dot->setVisible(active != end(state->data.active));
			control.reset->setDisabled(!state->data.saved.contains(route) && active == end(state->data.active));
		}
	};
	const auto load = [=] {
		state->data = query();
		refresh();
		return state->data.offers;
	};
	const auto save = [=](const QString &route, const QJsonObject &offer) {
		try {
			apply(route, offer);
			load();
			errorLabel->setText(QString());
		} catch (const std::exception &error) {
			errorLabel->setText(QString::fromUtf8(error.what()));
		}
	};
	struct Route {
		QString key;
		QString name;
		const style::icon *icon;
	};
	const auto routes = std::array{
		Route{ u"default"_q, u"Прокси по умолчанию"_q, &st::menuIconNetwork },
		Route{ u"api"_q, u"Запросы Telegram"_q, &st::menuIconSend },
		Route{ u"files"_q, u"Загрузка и отправка файлов"_q, &st::menuIconDownload },
		Route{ u"pages"_q, u"Встроенные веб-страницы"_q, &st::menuIconLink },
		Route{ u"mini_apps"_q, u"Mini Apps"_q, &st::menuIconBot },
	};
	for (const auto &[route, name, icon] : routes) {
		auto line = object_ptr<Ui::RpWidget>(box);
		const auto row = line.data();
		row->resize(1, style::ConvertScale(64));
		const auto title = Ui::CreateChild<Ui::FlatLabel>(row, name, RouteLabelStyle());
		const auto select = Ui::CreateChild<Ui::RoundButton>(row, rpl::single(QString()), SelectorStyle());
		const auto reset = Ui::CreateChild<Ui::RoundButton>(row, rpl::single(u"↶"_q), SelectorStyle());
		const auto dot = Ui::CreateChild<Ui::RpWidget>(select);
		dot->setAttribute(Qt::WA_TransparentForMouseEvents);
		dot->resize(style::ConvertScale(7), style::ConvertScale(7));
		dot->paintRequest() | rpl::on_next([=](QRect) {
			auto p = QPainter(dot);
			p.setRenderHint(QPainter::Antialiasing);
			p.setPen(Qt::NoPen);
			p.setBrush(st::groupCallLive1);
			p.drawEllipse(dot->rect());
		}, dot->lifetime());
		select->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		reset->setAccessibleName(u"Сбросить маршрут: "_q + name);
		reset->setToolTip(u"Восстановить настройки"_q);
		reset->setTextFgOverride(st::windowSubTextFg->c);
		state->controls.emplace(route, Control{select, reset, dot});
		row->widthValue() | rpl::on_next([=](int width) {
			const auto gap = style::ConvertScale(8);
			const auto left = style::ConvertScale(45);
			reset->setFullWidth(style::ConvertScale(38));
			select->setFullWidth(std::max((width - reset->width() - 2 * gap) * 55 / 100, 1));
			const auto selectLeft = width - reset->width() - gap - select->width();
			title->resizeToWidth(std::max(selectLeft - left - gap, 1));
			title->moveToLeft(left, (row->height() - title->height()) / 2);
			select->moveToLeft(selectLeft, (row->height() - select->height()) / 2);
			dot->moveToLeft(gap, (select->height() - dot->height()) / 2);
			reset->moveToRight(0, (row->height() - reset->height()) / 2);
		}, row->lifetime());
		row->paintRequest() | rpl::on_next([=](QRect) {
			auto p = QPainter(row);
			icon->paint(p, 0, (row->height() - icon->height()) / 2, row->width());
			p.fillRect(0, row->height() - st::lineWidth, row->width(), st::lineWidth, st::shadowFg);
		}, row->lifetime());
		reset->setClickedCallback([=] { save(route, {}); });
		select->setClickedCallback([=] {
			try {
				const auto offers = load();
				state->menu = base::make_unique_q<Ui::PopupMenu>(select, st::defaultPopupMenu);
				state->menu->addAction(u"По умолчанию"_q, [=] { save(route, {}); });
				for (const auto &entry : offers) {
					const auto offer = entry.toObject();
					if (!offer.value(u"routes"_q).toArray().contains(route)) {
						continue;
					}
					const auto token = offer.value(u"token"_q).toString();
					const auto name = PluginProxyLabel(offer);
					state->menu->addAction(name, [=] {
						try {
							const auto current = load();
							for (const auto &value : current) {
								const auto candidate = value.toObject();
								if (candidate.value(u"token"_q).toString() == token
								&& candidate.value(u"routes"_q).toArray().contains(route)) {
									save(route, candidate);
									return;
								}
							}
							errorLabel->setText(u"Прокси остановлен. Запустите его и выберите снова."_q);
						} catch (const std::exception &error) {
							errorLabel->setText(QString::fromUtf8(error.what()));
						}
					});
				}
				state->menu->popup(select->mapToGlobal(QPoint(0, select->height())));
			} catch (const std::exception &error) {
				errorLabel->setText(QString::fromUtf8(error.what()));
			}
		});
		box->addRow(std::move(line), style::al_justify);
	}
	try {
		load();
	} catch (const std::exception &error) {
		errorLabel->setText(QString::fromUtf8(error.what()));
	}
	auto resetAll = box->addLeftButton(rpl::single(u"Сбросить все"_q), [=] { save({}, {}); });
	resetAll->setTextFgOverride(st::attentionButtonFg->c);
	box->addButton(rpl::single(u"Готово"_q), [=] { box->closeBox(); }, st::defaultActiveButton);
}

} // namespace Ovygram
