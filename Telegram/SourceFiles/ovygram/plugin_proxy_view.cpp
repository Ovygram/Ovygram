#include "ovygram/plugin_proxy_view.h"

#include "base/unique_qptr.h"
#include "ovygram/i18n.h"
#include "ovygram/plugin_trust_badge.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"
#include "styles/style_plugin_management.h"

#include <QtGui/QPainterPath>

#include <algorithm>
#include <array>
#include <exception>

namespace Ovygram {
namespace {

class ProxySelectorButton final : public Ui::RippleButton {
public:
	explicit ProxySelectorButton(QWidget *parent)
	: Ui::RippleButton(parent, st::settingsPluginProxySelector.ripple) {
		resize(width(), st::settingsPluginProxySelector.height);
		style::PaletteChanged() | rpl::on_next([this] { update(); }, lifetime());
	}

	void setValue(QString text, bool active) {
		_text = std::move(text);
		_active = active;
		setToolTip(_text);
		accessibilityNameChanged();
		update();
	}

	QString accessibilityName() override {
		return _text;
	}

protected:
	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		const auto quality = PainterHighQualityEnabler(p);
		const auto over = (isOver() || isDown()) && !isDisabled();
		const auto radius = st::settingsPluginProxyRadius;
		const auto fill = over
			? st::settingsPluginRowPressed->c
			: st::settingsPluginProxySelector.textBg->c;
		auto border = st::windowFg->c;
		border.setAlphaF(st::settingsPluginInstallOutlineAlpha / 100.);
		const auto inset = style::ConvertScale(1) / 2.;
		const auto bounds = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
		auto path = QPainterPath();
		path.addRoundedRect(bounds, radius, radius);
		p.fillPath(path, fill);
		p.save();
		p.setClipPath(path);
		paintRipple(p, 0, 0);
		p.restore();
		p.setPen(QPen(border, style::ConvertScale(1)));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);

		const auto sidePad = style::ConvertScale(12);
		const auto chevronWidth = style::ConvertScale(10);
		const auto chevronGap = style::ConvertScale(8);
		auto textLeft = sidePad;
		if (_active) {
			const auto dotSize = style::ConvertScale(7);
			const auto dotRect = style::rtlrect(
				sidePad,
				(height() - dotSize) / 2,
				dotSize,
				dotSize,
				width());
			p.setPen(Qt::NoPen);
			p.setBrush(PluginTrustColor(u"verified"_q));
			p.drawEllipse(dotRect);
			textLeft += dotSize + style::ConvertScale(8);
		}
		const auto textRight = width() - sidePad - chevronWidth - chevronGap;
		const auto textWidth = std::max(textRight - textLeft, 0);
		const auto &font = st::settingsPluginProxySelector.style.font;
		if (textWidth > 0 && !_text.isEmpty()) {
			p.setFont(font);
			p.setPen(over
				? st::settingsPluginProxySelector.textFgOver
				: st::settingsPluginProxySelector.textFg);
			const auto elided = font->elided(_text, textWidth);
			p.drawTextLeft(textLeft, (height() - font->height) / 2, width(), elided);
		}

		// Native vector dropdown chevron on the trailing edge.
		const auto chevronLeft = style::RightToLeft()
			? sidePad
			: (width() - sidePad - chevronWidth);
		const auto midY = height() / 2.;
		const auto halfH = style::ConvertScale(3) * 0.85;
		auto chevronPen = QPen(st::windowSubTextFg->c, style::ConvertScale(1.6));
		chevronPen.setCapStyle(Qt::RoundCap);
		chevronPen.setJoinStyle(Qt::RoundJoin);
		p.setPen(chevronPen);
		p.setBrush(Qt::NoBrush);
		auto chevron = QPainterPath();
		chevron.moveTo(chevronLeft, midY - halfH);
		chevron.lineTo(chevronLeft + chevronWidth / 2., midY + halfH);
		chevron.lineTo(chevronLeft + chevronWidth, midY - halfH);
		p.drawPath(chevron);
	}

	QImage prepareRippleMask() const override {
		return Ui::RippleAnimation::RoundRectMask(size(), st::settingsPluginProxyRadius);
	}

private:
	QString _text;
	bool _active = false;
};

class ProxyRouteRow final : public Ui::RpWidget {
public:
	ProxyRouteRow(
		QWidget *parent,
		const QString &name,
		not_null<const style::icon*> icon)
	: Ui::RpWidget(parent)
	, _icon(icon)
	, _title(Ui::CreateChild<Ui::FlatLabel>(this, name, st::settingsPluginBody))
	, _select(Ui::CreateChild<ProxySelectorButton>(this))
	, _reset(Ui::CreateChild<Ui::RoundButton>(
		this,
		rpl::single(u"↶"_q),
		st::settingsPluginProxyReset)) {
		_reset->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		_reset->setAccessibleName(Tr(u"Reset route: "_q, u"Сбросить маршрут: "_q) + name);
		_reset->setToolTip(Tr(u"Restore defaults"_q, u"Восстановить настройки"_q));
		const auto updateResetStyle = [this] {
			auto border = st::windowFg->c;
			border.setAlphaF(st::settingsPluginInstallOutlineAlpha / 100.);
			_reset->setPenOverride(QPen(border, st::lineWidth));
		};
		updateResetStyle();
		style::PaletteChanged() | rpl::on_next(updateResetStyle, _reset->lifetime());
	}

	not_null<ProxySelectorButton*> select() const { return _select; }
	not_null<Ui::RoundButton*> reset() const { return _reset; }

protected:
	int resizeGetHeight(int width) override {
		const auto gap = style::ConvertScale(8);
		const auto vPad = style::ConvertScale(12);
		const auto iconSlot = style::ConvertScale(36);
		const auto resetWidth = st::settingsPluginProxyResetWidth;
		_reset->setFullWidth(resetWidth);
		const auto stacked = width < style::ConvertScale(460);
		if (stacked) {
			const auto titleWidth = std::max(width - iconSlot, 1);
			_title->resizeToWidth(titleWidth);
			const auto topRowHeight = std::max(_title->height(), _icon->height());
			_title->moveToLeft(iconSlot, vPad + (topRowHeight - _title->height()) / 2, width);
			_iconY = vPad + (topRowHeight - _icon->height()) / 2;
			const auto controlsTop = vPad + topRowHeight + gap;
			const auto selectWidth = std::max(width - resetWidth - gap, 1);
			_select->resize(selectWidth, _select->height());
			_select->moveToLeft(0, controlsTop, width);
			_reset->moveToRight(0, controlsTop + (_select->height() - _reset->height()) / 2, width);
			return controlsTop + std::max(_select->height(), _reset->height()) + vPad;
		}
		const auto controlsWidth = std::clamp(
			(width - iconSlot) * 56 / 100,
			style::ConvertScale(200),
			std::max(width - iconSlot - style::ConvertScale(150), style::ConvertScale(200)));
		const auto selectWidth = std::max(controlsWidth - resetWidth - gap, 1);
		const auto selectLeft = width - resetWidth - gap - selectWidth;
		const auto titleWidth = std::max(selectLeft - iconSlot - gap, 1);
		_title->resizeToWidth(titleWidth);
		_select->resize(selectWidth, _select->height());
		const auto rowHeight = std::max({
			_title->height(),
			_select->height(),
			_reset->height(),
			_icon->height(),
			style::ConvertScale(36),
		}) + 2 * vPad;
		_iconY = (rowHeight - _icon->height()) / 2;
		_title->moveToLeft(iconSlot, (rowHeight - _title->height()) / 2, width);
		_select->moveToLeft(selectLeft, (rowHeight - _select->height()) / 2, width);
		_reset->moveToRight(0, (rowHeight - _reset->height()) / 2, width);
		return rowHeight;
	}

	void paintEvent(QPaintEvent *) override {
		auto p = QPainter(this);
		_icon->paint(p, 0, _iconY, width());
		auto divider = st::windowFg->c;
		divider.setAlphaF(st::settingsPluginDividerAlpha);
		p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, divider);
	}

private:
	const not_null<const style::icon*> _icon;
	const not_null<Ui::FlatLabel*> _title;
	const not_null<ProxySelectorButton*> _select;
	const not_null<Ui::RoundButton*> _reset;
	int _iconY = 0;
};

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
	box->setTitle(rpl::single(Tr(u"Plugin proxies"_q, u"Прокси плагинов"_q)));
	box->addTopButton(st::settingsPluginClose, [=] { box->closeBox(); })
		->setAccessibleName(Tr(u"Close"_q, u"Закрыть"_q));
	const auto padding = st::boxRowPadding;
	const auto subtitle = box->addRow(object_ptr<Ui::FlatLabel>(box,
		Tr(u"Assign proxies by traffic type"_q, u"Назначение прокси по типам трафика"_q),
		st::settingsPluginBody),
		QMargins(padding.left(), 0, padding.right(), style::ConvertScale(14)));
	const auto updateSubtitleColor = [=] {
		subtitle->setTextColorOverride(st::settingsPluginLabelMuted->c);
	};
	updateSubtitleColor();
	style::PaletteChanged() | rpl::on_next(updateSubtitleColor, subtitle->lifetime());

	auto divider = object_ptr<Ui::RpWidget>(box);
	const auto line = divider.data();
	line->resize(1, st::lineWidth);
	line->paintRequest() | rpl::on_next([=](QRect rect) {
		auto color = st::windowFg->c;
		color.setAlphaF(st::settingsPluginDividerAlpha);
		QPainter(line).fillRect(rect, color);
	}, line->lifetime());
	box->addRow(std::move(divider), QMargins(), style::al_justify);

	struct Control {
		QPointer<ProxySelectorButton> select;
		QPointer<Ui::RoundButton> reset;
	};
	struct State {
		std::map<QString, Control> controls;
		ProxyViewState data;
		base::unique_qptr<Ui::PopupMenu> menu;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto errorLabel = box->addRow(
		object_ptr<Ui::FlatLabel>(box, QString(), st::settingsPluginBody),
		QMargins(padding.left(), style::ConvertScale(6), padding.right(), 0));
	errorLabel->setVisible(false);
	const auto updateErrorColor = [=] {
		errorLabel->setTextColorOverride(st::settingsPluginLabelAttention->c);
	};
	updateErrorColor();
	style::PaletteChanged() | rpl::on_next(updateErrorColor, errorLabel->lifetime());
	const auto setError = [=](const QString &text) {
		errorLabel->setText(text);
		errorLabel->setVisible(!text.isEmpty());
	};

	const auto refresh = [=] {
		for (const auto &[route, control] : state->controls) {
			const auto active = state->data.active.find(route);
			const auto isActive = (active != end(state->data.active));
			const auto text = isActive
				? (active->second + Tr(u" (connected)"_q, u" (подключён)"_q))
				: (state->data.saved.contains(route)
					? Tr(u"Waiting to start"_q, u"Ожидает запуска"_q)
					: Tr(u"Default"_q, u"По умолчанию"_q));
			control.select->setValue(text, isActive);
			control.reset->setDisabled(!state->data.saved.contains(route) && !isActive);
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
			setError(QString());
		} catch (const std::exception &error) {
			setError(QString::fromUtf8(error.what()));
		}
	};
	struct Route {
		QString key;
		QString name;
		const style::icon *icon;
	};
	const auto routes = std::array{
		Route{ u"default"_q, Tr(u"Default proxy"_q, u"Прокси по умолчанию"_q), &st::menuIconNetwork },
		Route{ u"api"_q, Tr(u"Telegram requests"_q, u"Запросы Telegram"_q), &st::menuIconSend },
		Route{ u"files"_q, Tr(u"File downloads and uploads"_q, u"Загрузка и отправка файлов"_q), &st::menuIconDownload },
		Route{ u"pages"_q, Tr(u"Built-in web pages"_q, u"Встроенные веб-страницы"_q), &st::menuIconLink },
		Route{ u"mini_apps"_q, u"Mini Apps"_q, &st::menuIconBot },
	};
	for (const auto &[route, name, icon] : routes) {
		auto rowWidget = object_ptr<ProxyRouteRow>(box, name, icon);
		const auto select = rowWidget->select();
		const auto reset = rowWidget->reset();
		state->controls.emplace(route, Control{ select.get(), reset.get() });
		reset->setClickedCallback([=] { save(route, {}); });
		select->setClickedCallback([=] {
			try {
				const auto offers = load();
				state->menu = base::make_unique_q<Ui::PopupMenu>(select, st::settingsPluginPopup);
				state->menu->addAction(Tr(u"Default"_q, u"По умолчанию"_q), [=] { save(route, {}); });
				for (const auto &entry : offers) {
					const auto offer = entry.toObject();
					if (!offer.value(u"routes"_q).toArray().contains(route)) {
						continue;
					}
					const auto token = offer.value(u"token"_q).toString();
					const auto label = PluginProxyLabel(offer);
					state->menu->addAction(label, [=] {
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
							setError(Tr(
								u"The proxy has stopped. Start it and select it again."_q,
								u"Прокси остановлен. Запустите его и выберите снова."_q));
						} catch (const std::exception &error) {
							setError(QString::fromUtf8(error.what()));
						}
					});
				}
				state->menu->popup(select->mapToGlobal(QPoint(0, select->height())));
			} catch (const std::exception &error) {
				setError(QString::fromUtf8(error.what()));
			}
		});
		box->addRow(std::move(rowWidget), style::al_justify);
	}
	try {
		load();
	} catch (const std::exception &error) {
		setError(QString::fromUtf8(error.what()));
	}
	box->addLeftButton(rpl::single(Tr(u"Reset all"_q, u"Сбросить все"_q)), [=] { save({}, {}); }, st::settingsPluginDialogDanger);
	box->addButton(rpl::single(Tr(u"Done"_q, u"Готово"_q)), [=] { box->closeBox(); }, st::settingsPluginPrimary);
}

} // namespace Ovygram
