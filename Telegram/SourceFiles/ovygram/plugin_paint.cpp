#include "ovygram/plugin_paint.h"
#include <QtCore/QJsonObject>

#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Ovygram {
namespace {

constexpr auto kMaxCoordinate = 16384.;
constexpr auto kMaxLineWidth = 64.;
constexpr auto kMaxPaintTextLength = 1024;

double Resolve(double value, int size) {
	return (value < 0) ? (size + value) : value;
}

} // namespace

QColor ParsePaintColor(const QJsonValue &value) {
	const auto result = QColor(value.toString());
	if (!result.isValid()) {
		throw std::runtime_error("Invalid color, use #rrggbb or #aarrggbb.");
	}
	return result;
}

std::vector<PaintOp> ParsePaintOps(const QJsonArray &commands, int limit) {
	if (commands.size() > limit) {
		throw std::runtime_error("Too many draw commands.");
	}
	auto result = std::vector<PaintOp>();
	for (const auto &value : commands) {
		const auto command = value.toObject();
		const auto name = command.value(u"op"_q).toString();
		const auto number = [&](const char *key, double fallback = 0.) {
			const auto value = command.value(QLatin1String(key)).toDouble(fallback);
			if (!std::isfinite(value)) {
				throw std::runtime_error("Draw coordinates must be finite numbers.");
			}
			return value;
		};
		auto text = command.value(u"text"_q).toString();
		if (text.size() > kMaxPaintTextLength) {
			text.truncate(kMaxPaintTextLength);
		}
		auto op = PaintOp{
			.x = std::clamp(number("x"), -kMaxCoordinate, kMaxCoordinate),
			.y = std::clamp(number("y"), -kMaxCoordinate, kMaxCoordinate),
			.width = std::clamp(number("width"), 0., kMaxCoordinate),
			.height = std::clamp(number("height"), 0., kMaxCoordinate),
			.x2 = std::clamp(number("x2"), -kMaxCoordinate, kMaxCoordinate),
			.y2 = std::clamp(number("y2"), -kMaxCoordinate, kMaxCoordinate),
			.radius = std::clamp(number("radius"), 0., kMaxCoordinate),
			.lineWidth = std::clamp(number("width", 1.), 1., kMaxLineWidth),
			.size = int(std::clamp(number("size", 13.), 6., 96.)),
			.color = ParsePaintColor(command.value(u"color"_q)),
			.text = std::move(text),
		};
		if (name == u"rect"_q) {
			op.type = PaintOp::Type::Rect;
		} else if (name == u"ellipse"_q) {
			op.type = PaintOp::Type::Ellipse;
		} else if (name == u"line"_q) {
			op.type = PaintOp::Type::Line;
			op.x = std::clamp(number("x1"), -kMaxCoordinate, kMaxCoordinate);
			op.y = std::clamp(number("y1"), -kMaxCoordinate, kMaxCoordinate);
		} else if (name == u"text"_q) {
			op.type = PaintOp::Type::Text;
		} else {
			throw std::runtime_error("Unknown draw command.");
		}
		result.push_back(std::move(op));
	}
	return result;
}

void PaintOps(QPainter &p, const std::vector<PaintOp> &ops, QRect row) {
	if (ops.empty() || row.isEmpty()) {
		return;
	}
	p.save();
	p.setClipRect(row, Qt::IntersectClip);
	for (const auto &op : ops) {
		const auto x = row.x() + Resolve(op.x, row.width());
		const auto y = row.y() + Resolve(op.y, row.height());
		switch (op.type) {
		case PaintOp::Type::Rect:
			p.setPen(Qt::NoPen);
			p.setBrush(op.color);
			p.drawRoundedRect(
				QRectF(x, y, op.width, op.height),
				op.radius,
				op.radius);
			break;
		case PaintOp::Type::Ellipse:
			p.setPen(Qt::NoPen);
			p.setBrush(op.color);
			p.drawEllipse(QRectF(x, y, op.width, op.height));
			break;
		case PaintOp::Type::Line:
			p.setPen(QPen(op.color, op.lineWidth));
			p.drawLine(QPointF(x, y), QPointF(
				row.x() + Resolve(op.x2, row.width()),
				row.y() + Resolve(op.y2, row.height())));
			break;
		case PaintOp::Type::Text: {
			auto font = p.font();
			font.setPixelSize(op.size);
			p.setFont(font);
			p.setPen(op.color);
			p.drawText(QPointF(x, y), op.text);
		} break;
		}
	}
	p.restore();
}

} // namespace Ovygram
