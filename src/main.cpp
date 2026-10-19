// Precise Inputs - Geode mod for Geometry Dash 2.2081
//
// Behaviour:
//  - When you press jump (button 1) the mod predicts, tick by tick, whether
//    clicking NOW would get the player killed (spike / side-crash into a block).
//  - If clicking now is safe            -> click registers immediately.
//  - If a safe tick exists within 1-3   -> the click is delayed to that tick.
//  - If no safe tick exists in window   -> click registers immediately (no delay),
//                                          so you just die like normal.
//
// v1 limitation: cube gamemode only, player 1 only. Everything else passes through.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

using namespace geode::prelude;

namespace pi {

	constexpr int    kHorizon       = 36;
	constexpr float  kHazardScale   = 0.4f;
	constexpr float  kLandTolerance = 10.f;
	constexpr double kDefaultJump   = 11.180032;
	constexpr double kFallClamp     = 15.0;

	struct Pending {
		int  ticksLeft;
		bool down;
		int  button;
		bool p1;
	};

	struct Obstacle {
		CCRect rect;
		bool   hazard;
	};

	struct State {
		std::deque<Pending> queue;
		bool dispatching = false;

		bool    haveLast = false;
		CCPoint lastPos{0, 0};
		double  lastVy = 0;
		bool    lastGrounded = true;

		bool   haveG = false;
		double absG = 0;
		double k = 0;
		double dx = 0;
		double learnedJump = 0;
	};

	inline State& st() {
		static State s;
		return s;
	}

	inline bool enabled() {
		return Mod::get()->getSettingValue<bool>("active");
	}

	inline int bufferTicks() {
		return static_cast<int>(std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("buffer-ticks"), 1, 3));
	}

	inline void setEnabled(bool v) {
		Mod::get()->setSettingValue<bool>("active", v);
		if (!v) st().queue.clear();
	}

	inline void reset() {
		auto& s = st();
		s.queue.clear();
		s.dispatching = false;
		s.haveLast = false;
	}

	inline bool isCube(PlayerObject* p) {
		return !(p->m_isShip || p->m_isBird || p->m_isBall || p->m_isDart ||
		         p->m_isRobot || p->m_isSpider || p->m_isSwing);
	}

	inline bool intersects(CCRect const& a, CCRect const& b) {
		return a.getMinX() < b.getMaxX() && a.getMaxX() > b.getMinX() &&
		       a.getMinY() < b.getMaxY() && a.getMaxY() > b.getMinY();
	}

	inline void record(PlayerObject* p) {
		auto& s = st();
		CCPoint pos = p->getPosition();
		double vy = p->m_yVelocity;
		bool grounded = p->m_isOnGround;
		double dir = p->m_isUpsideDown ? -1.0 : 1.0;

		if (s.haveLast) {
			double ddx = pos.x - s.lastPos.x;
			if (ddx > 0.0 && ddx < 40.0) s.dx = ddx;

			bool bothAir = !grounded && !s.lastGrounded;
			double dv = vy - s.lastVy;
			if (bothAir && std::abs(vy) < kFallClamp - 0.5 && std::abs(s.lastVy) < kFallClamp - 0.5 && dv * dir < -1e-5) {
				double avg = (vy + s.lastVy) * 0.5;
				double dyo = pos.y - s.lastPos.y;
				if (std::abs(avg) > 0.5 && dyo / avg > 0.0) {
					s.absG = -dv * dir;
					s.k = dyo / avg;
					s.haveG = true;
				}
			}

			if (s.haveG && s.lastGrounded && !grounded && isCube(p) && vy * dir > 3.0) {
				s.learnedJump = std::clamp(std::abs(vy) + s.absG, 8.0, 16.0);
			}
		}

		s.lastPos = pos;
		s.lastVy = vy;
		s.lastGrounded = grounded;
		s.haveLast = true;
	}

	inline std::vector<Obstacle> collect(GJBaseGameLayer* layer, float xMin, float xMax) {
		std::vector<Obstacle> out;
		for (auto obj : CCArrayExt<GameObject*>(layer->m_objects)) {
			if (!obj) continue;
			float ox = obj->getPositionX();
			if (ox < xMin || ox > xMax) continue;
			auto t = obj->m_objectType;
			if (t != GameObjectType::Hazard && t != GameObjectType::Solid) continue;
			out.push_back({ obj->getObjectRect(), t == GameObjectType::Hazard });
		}
		return out;
	}

	struct SimInput {
		CCRect rect;
		double vy, absG, k, dx, jump;
		float  dir;
		bool   grounded;
	};

	inline bool survives(SimInput const& s, int clickTick, std::vector<Obstacle> const& obs) {
		CCRect r = s.rect;
		double vy = s.vy;
		bool grounded = s.grounded;
		bool held = false;
		double g = -static_cast<double>(s.dir) * s.absG;

		for (int t = 0; t <= kHorizon; ++t) {
			if (t == clickTick) held = true;

			bool wasGrounded = grounded;
			if (held && grounded) {
				vy = s.dir * s.jump;
				grounded = false;
				wasGrounded = false;
			}

			double vyOld = vy;
			if (!grounded) {
				vy = std::clamp(vy + g, -kFallClamp, kFallClamp);
			} else {
				vy = 0;
			}
			double dy = grounded ? 0.0 : s.k * (vyOld + vy) * 0.5;

			r.origin.x += static_cast<float>(s.dx);
			r.origin.y += static_cast<float>(dy);

			bool landed = false;
			for (auto const& o : obs) {
				if (o.hazard) {
					CCRect h = r;
					float cx = r.getMidX(), cy = r.getMidY();
					h.size.width  *= kHazardScale;
					h.size.height *= kHazardScale;
					h.origin.x = cx - h.size.width / 2;
					h.origin.y = cy - h.size.height / 2;
					if (intersects(h, o.rect)) return false;
					continue;
				}
				if (!intersects(r, o.rect)) continue;

				float feetPen = s.dir > 0 ? (o.rect.getMaxY() - r.getMinY()) : (r.getMaxY() - o.rect.getMinY());
				float headPen = s.dir > 0 ? (r.getMaxY() - o.rect.getMinY()) : (o.rect.getMaxY() - r.getMinY());
				bool movingDown = vy * s.dir <= 0.0;

				if (feetPen <= kLandTolerance && movingDown) {
					r.origin.y += s.dir * feetPen;
					vy = 0;
					landed = true;
				} else if (headPen <= kLandTolerance) {
					r.origin.y -= s.dir * headPen;
					vy = 0;
				} else {
					return false;
				}
			}

			if (landed) {
				grounded = true;
			} else if (wasGrounded) {
				CCRect probe = r;
				probe.origin.y -= s.dir * 1.0f;
				bool supported = false;
				for (auto const& o : obs) {
					if (!o.hazard && intersects(probe, o.rect)) { supported = true; break; }
				}
				grounded = supported;
			}
		}
		return true;
	}

	inline int pickDelay(GJBaseGameLayer* layer, PlayerObject* p, int window) {
		auto& s = st();
		if (!p || !isCube(p)) return 0;
		if (!s.haveG || s.absG < 1e-6 || s.k < 1e-6 || s.dx <= 0.0) return 0;

		SimInput in;
		in.rect = p->getObjectRect();
		in.vy = p->m_yVelocity;
		in.absG = s.absG;
		in.k = s.k;
		in.dx = s.dx;
		in.jump = s.learnedJump > 0 ? s.learnedJump : kDefaultJump;
		in.dir = p->m_isUpsideDown ? -1.f : 1.f;
		in.grounded = p->m_isOnGround;

		float xMin = in.rect.getMinX() - 60.f;
		float xMax = in.rect.getMaxX() + static_cast<float>(s.dx * (kHorizon + 1)) + 60.f;
		auto obs = collect(layer, xMin, xMax);

		for (int k = 0; k <= window; ++k) {
			if (survives(in, k, obs)) return k;
		}
		return 0;
	}

	inline void dispatchDue(PlayLayer* layer) {
		auto& s = st();
		auto& q = s.queue;
		while (!q.empty() && q.front().ticksLeft <= 0) {
			Pending e = q.front();
			q.pop_front();
			s.dispatching = true;
			layer->handleButton(e.down, e.button, e.p1);
			s.dispatching = false;
		}
		for (auto& e : q) e.ticksLeft--;
	}

} // namespace pi

class $modify(PIBaseLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		auto& s = pi::st();
		auto pl = PlayLayer::get();

		if (s.dispatching || !pl || static_cast<GJBaseGameLayer*>(pl) != this ||
		    button != 1 || !isPlayer1 || !pi::enabled()) {
			return GJBaseGameLayer::handleButton(down, button, isPlayer1);
		}

		if (!s.queue.empty()) {
			s.queue.push_back({ s.queue.back().ticksLeft + 1, down, button, isPlayer1 });
			return;
		}

		if (!down) {
			return GJBaseGameLayer::handleButton(down, button, isPlayer1);
		}

		int delay = pi::pickDelay(this, m_player1, pi::bufferTicks());
		if (delay <= 0) {
			return GJBaseGameLayer::handleButton(down, button, isPlayer1);
		}
		s.queue.push_back({ delay, true, button, true });
	}
};

class $modify(PIPlayer, PlayerObject) {
	void update(float dt) {
		auto pl = PlayLayer::get();
		bool isP1 = pl && this == pl->m_player1;

		if (isP1) {
			if (pi::enabled()) pi::dispatchDue(pl);
			else pi::st().queue.clear();
		}

		PlayerObject::update(dt);

		if (isP1) pi::record(this);
	}
};

class $modify(PIPlayLayer, PlayLayer) {
	void resetLevel() {
		pi::reset();
		PlayLayer::resetLevel();
	}
};

class $modify(PIPauseLayer, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();

		auto menu = CCMenu::create();
		menu->setID("precise-inputs-menu"_spr);
		menu->setPosition({ 0.f, 0.f });

		auto toggler = CCMenuItemToggler::createWithStandardSprites(
			this, menu_selector(PIPauseLayer::onTogglePreciseInputs), 0.6f
		);
		toggler->toggle(pi::enabled());
		toggler->setPosition({ 32.f, 44.f });
		menu->addChild(toggler);

		auto label = CCLabelBMFont::create("Precise Inputs", "bigFont.fnt");
		label->setScale(0.4f);
		label->setAnchorPoint({ 0.f, 0.5f });
		label->setPosition({ 52.f, 44.f });
		menu->addChild(label);

		this->addChild(menu);
	}

	void onTogglePreciseInputs(CCObject* sender) {
		bool nowOn = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
		pi::setEnabled(nowOn);
	}
};

$execute {
	listenForKeybindSettingPresses("toggle-key", [](Keybind const&, bool down, bool repeat, double) {
		if (!down || repeat) return;
		bool now = !pi::enabled();
		pi::setEnabled(now);
		Notification::create(
			now ? "Precise Inputs: ON" : "Precise Inputs: OFF",
			now ? NotificationIcon::Success : NotificationIcon::Error,
			1.f
		)->show();
	}, Mod::get());
}
