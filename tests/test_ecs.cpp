// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <cstring>
#include <vector>

#include <ironclad/components.hpp>
#include <ironclad/ecs.hpp>
#include <ironclad/hash.hpp>

using namespace ironclad;

// Build a small world that exercises every demo component type.
static World make_world() {
    World w(64);
    w.register_component<Transform>();
    w.register_component<Velocity>();
    w.register_component<Player>();
    w.register_component<Projectile>();
    w.register_component<Hitbox>();

    for (int i = 0; i < 4; ++i) {
        Entity e = w.create();
        w.add(e, Transform{Vec2{Fixed{i * 10}, Fixed{i * 5}},
                           Fixed::from_ratio(i, 7)});
        w.add(e, Velocity{Vec2{Fixed::from_ratio(1, 2), Fixed{0}}});
        w.add(e, Player{static_cast<std::uint8_t>(i),
                        static_cast<std::uint8_t>(i),
                        0, 1, Fixed{100 - i}, static_cast<std::uint32_t>(i * 7)});
        w.add(e, Hitbox{Fixed::from_ratio(3, 4)});
    }
    // A few projectiles
    for (int i = 0; i < 3; ++i) {
        Entity p = w.create();
        w.add(p, Transform{Vec2{Fixed{i}, Fixed{-i}}, Fixed{}});
        w.add(p, Velocity{Vec2{Fixed{0}, Fixed{2}}});
        w.add(p, Projectile{static_cast<std::uint32_t>(i),
                            static_cast<std::uint16_t>(60 - i), 0});
        w.add(p, Hitbox{Fixed::from_ratio(1, 4)});
    }
    return w;
}

TEST_CASE("entity create/destroy/alive bookkeeping") {
    World w(8);
    Entity a = w.create();
    Entity b = w.create();
    CHECK(w.is_alive(a));
    CHECK(w.is_alive(b));
    CHECK(w.alive_count() == 2u);
    w.destroy(a);
    CHECK_FALSE(w.is_alive(a));
    CHECK(w.alive_count() == 1u);
    Entity c = w.create();
    CHECK(c == a);   // recycled id
    CHECK(w.is_alive(c));
}

TEST_CASE("add / has / get / remove component") {
    World w(8);
    w.register_component<Velocity>();
    Entity e = w.create();
    CHECK_FALSE(w.has_component<Velocity>(e));
    w.add<Velocity>(e, Velocity{Vec2{Fixed{1}, Fixed{2}}});
    CHECK(w.has_component<Velocity>(e));
    auto* v = w.get<Velocity>(e);
    REQUIRE(v != nullptr);
    CHECK(v->v == Vec2{Fixed{1}, Fixed{2}});
    w.remove<Velocity>(e);
    CHECK_FALSE(w.has_component<Velocity>(e));
    CHECK(w.get<Velocity>(e) == nullptr);
}

TEST_CASE("each iterates only alive entities with component, in id order") {
    World w(16);
    w.register_component<Player>();
    Entity a = w.create();   // 0
    Entity b = w.create();   // 1
    Entity c = w.create();   // 2
    w.add<Player>(a, Player{1, 0, 0, 1, Fixed{100}, 0});
    w.add<Player>(c, Player{3, 0, 0, 1, Fixed{100}, 0});
    w.destroy(b);
    std::vector<Entity> seen;
    w.each<Player>([&](Entity e, Player&) { seen.push_back(e); });
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == a);
    CHECK(seen[1] == c);
}

TEST_CASE("snapshot serialize/deserialize round-trips byte-identically") {
    World a = make_world();
    ByteWriter w;
    a.serialize(w);
    auto bytes_a = std::vector<std::uint8_t>(w.view().begin(), w.view().end());

    World b(64);
    b.register_component<Transform>();
    b.register_component<Velocity>();
    b.register_component<Player>();
    b.register_component<Projectile>();
    b.register_component<Hitbox>();
    ByteReader r(bytes_a.data(), bytes_a.size());
    REQUIRE(b.deserialize(r));

    ByteWriter w2;
    b.serialize(w2);
    auto bytes_b = std::vector<std::uint8_t>(w2.view().begin(), w2.view().end());
    CHECK(bytes_a == bytes_b);
}

TEST_CASE("snapshot hash is stable across save->load->save cycles") {
    World a = make_world();
    ByteWriter w1;
    a.serialize(w1);
    auto h1 = hash64(w1.view().data(), w1.view().size());

    World b(64);
    b.register_component<Transform>();
    b.register_component<Velocity>();
    b.register_component<Player>();
    b.register_component<Projectile>();
    b.register_component<Hitbox>();
    ByteReader r(w1.view().data(), w1.view().size());
    REQUIRE(b.deserialize(r));
    ByteWriter w2;
    b.serialize(w2);
    auto h2 = hash64(w2.view().data(), w2.view().size());

    CHECK(h1 == h2);
}

TEST_CASE("two independently-built equivalent worlds hash identically") {
    World a = make_world();
    World b = make_world();
    ByteWriter wa, wb;
    a.serialize(wa);
    b.serialize(wb);
    CHECK(hash64(wa.view().data(), wa.view().size()) ==
          hash64(wb.view().data(), wb.view().size()));
}

TEST_CASE("destroying an entity removes it from each() and from has_component") {
    World w(16);
    w.register_component<Player>();
    Entity a = w.create();
    Entity b = w.create();
    w.add<Player>(a, Player{1, 0, 0, 1, Fixed{100}, 0});
    w.add<Player>(b, Player{2, 0, 0, 1, Fixed{100}, 0});
    w.destroy(a);
    CHECK_FALSE(w.has_component<Player>(a));
    int count = 0;
    w.each<Player>([&](Entity, Player&) { ++count; });
    CHECK(count == 1);
}

TEST_CASE("rejecting a snapshot with a different capacity") {
    World a = make_world();
    ByteWriter w;
    a.serialize(w);

    World b(128);  // different capacity
    b.register_component<Transform>();
    b.register_component<Velocity>();
    b.register_component<Player>();
    b.register_component<Projectile>();
    b.register_component<Hitbox>();
    ByteReader r(w.view().data(), w.view().size());
    CHECK_FALSE(b.deserialize(r));
}
