#pragma once

#include "../source/common/tick_engine_compat.h"
#include "../source/sync/tick_mesh_core.h"
#include "../source/sync/tick_sync_core.h"
#include "../source/transport/enet_hosted_mesh_transport.h"
#include "../source/transport/enet_mesh_transport.h"
#include "../source/transport/tick_local_transport.h"

#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#include "tests/test_macros.h"

// Fuzzing and chaos tests: not matched by `--test-case="*TickSynchronizer*"`; run them with
// `--test-case="*TickSyncFuzz*"`, ideally on a build with `use_asan=yes use_ubsan=yes`. They record the real traffic of
// an honest peer, then an impostor takes that peer's place and sends the packets again, mutated.
//
// Environment: `TICK_FUZZ_SEEDS` (how many seeds, 3 by default), `TICK_FUZZ_SEED` (the first one, 1), `TICK_FUZZ_STEPS`
// (steps of 1/60 s per seed, 600).
//
// The two campaigns where a node of a distributed mesh misbehaves fail on some seeds: the mesh trusts its nodes, and
// nothing bounds the terms, the versions and the timeline one of them announces (`notes/audit-2026-10-01.md`). They
// stay as they are, to tell when that changes.

namespace TestTickFuzz {

static int fuzz_env(const char *p_name, int p_default) {
	const String value = OS::get_singleton()->get_environment(p_name);
	return value.is_valid_int() ? int(value.to_int()) : p_default;
}

struct Sample {
	int from = 0;
	int to = 0;
	int channel = 0;
	TickTransport::TransferMode mode = TickTransport::TRANSFER_MODE_RELIABLE;
	LocalVector<uint8_t> data;
};

// Records what an engine sends.
class TapTransport : public TickTransport {
	GDSOFTCLASS(TapTransport, TickTransport);

public:
	Ref<TickTransport> inner;
	LocalVector<Sample> *corpus = nullptr;

	virtual int get_local_peer_id() const override { return inner->get_local_peer_id(); }
	virtual bool is_peer_connected(int p_peer) const override { return inner->is_peer_connected(p_peer); }
	virtual void get_connected_peers(LocalVector<int> &r_peers) const override { inner->get_connected_peers(r_peers); }
	virtual int get_channel_count() const override { return inner->get_channel_count(); }
	virtual int get_max_payload_size() const override { return inner->get_max_payload_size(); }
	virtual Error send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) override {
		if (corpus && corpus->size() < 60000) {
			Sample sample;
			sample.from = inner->get_local_peer_id();
			sample.to = p_peer;
			sample.channel = p_channel;
			sample.mode = p_mode;
			sample.data.resize(p_size);
			memcpy(sample.data.ptr(), p_data, p_size);
			corpus->push_back(sample);
		}
		return inner->send(p_peer, p_channel, p_mode, p_data, p_size);
	}
	virtual void disconnect_peer(int p_peer) override { inner->disconnect_peer(p_peer); }
	virtual void poll() override { inner->poll(); }
	virtual bool pop_event(Event &r_event) override { return inner->pop_event(r_event); }
	virtual bool pop_packet(Packet &r_packet) override { return inner->pop_packet(r_packet); }
};

struct Fuzzer {
	RandomPCG rng;

	explicit Fuzzer(uint64_t p_seed) { rng.seed(p_seed * 7919 + 17); }
	uint32_t next() { return rng.rand(); }
	int range(int p_low, int p_high) { return p_low + int(next() % uint32_t(p_high - p_low + 1)); }
	bool chance(int p_percent) { return int(next() % 100) < p_percent; }

	uint32_t interesting(uint32_t p_frame) {
		switch (next() % 10) {
			case 0:
				return 0;
			case 1:
				return 1;
			case 2:
				return 0xFFFFFFFF;
			case 3:
				return 0x7FFFFFFF;
			case 4:
				return 0x80000000;
			case 5:
				return p_frame + uint32_t(range(-200, 200));
			case 6:
				return p_frame + uint32_t(range(-3, 3));
			case 7:
				return uint32_t(range(0, 70000));
			case 8:
				return uint32_t(range(0, 300));
			default:
				return next();
		}
	}

	void mutate(LocalVector<uint8_t> &r_bytes, uint32_t p_frame) {
		const int operations = range(1, 4);
		for (int i = 0; i < operations; i++) {
			if (r_bytes.is_empty()) {
				r_bytes.push_back(uint8_t(next()));
				continue;
			}
			const uint32_t position = next() % r_bytes.size();
			switch (next() % 9) {
				case 0:
					r_bytes[position] ^= uint8_t(1u << (next() % 8));
					break;
				case 1:
					r_bytes[position] = uint8_t(next());
					break;
				case 2: {
					static const uint8_t values[5] = { 0x00, 0xFF, 0x7F, 0x80, 0x01 };
					r_bytes[position] = values[next() % 5];
				} break;
				case 3:
					r_bytes.resize(position + 1);
					break;
				case 4: {
					const int extra = range(1, 64);
					for (int b = 0; b < extra; b++) {
						r_bytes.push_back(uint8_t(next()));
					}
				} break;
				case 5: {
					const uint32_t value = interesting(p_frame);
					for (uint32_t b = 0; b < 4 && position + b < r_bytes.size(); b++) {
						r_bytes[position + b] = uint8_t(value >> (8 * b));
					}
				} break;
				case 6: {
					// A chunk again, at the end.
					const uint32_t length = MIN(uint32_t(range(1, 200)), r_bytes.size() - position);
					for (uint32_t b = 0; b < length; b++) {
						r_bytes.push_back(r_bytes[position + b]);
					}
				} break;
				case 7:
					// Another message type.
					r_bytes[0] = uint8_t(range(0, 40));
					break;
				default: {
					// A run of the same byte.
					const uint8_t value = uint8_t(next());
					const uint32_t length = MIN(uint32_t(range(1, 32)), r_bytes.size() - position);
					for (uint32_t b = 0; b < length; b++) {
						r_bytes[position + b] = value;
					}
				} break;
			}
		}
		if (r_bytes.size() > 60000) {
			r_bytes.resize(60000);
		}
	}

	void random_packet(LocalVector<uint8_t> &r_bytes) {
		const int size = chance(20) ? range(1, 4) : (chance(10) ? range(1000, 20000) : range(1, 400));
		r_bytes.resize(size);
		for (int i = 0; i < size; i++) {
			r_bytes[i] = uint8_t(next());
		}
		if (chance(70)) {
			r_bytes[0] = uint8_t(range(1, 34));
		}
	}

	Variant random_payload() {
		switch (next() % 14) {
			case 0:
				return Variant();
			case 1:
				return int64_t(next()) - 2000000000;
			case 2:
				return double(next()) / 1000.0 - 100.0;
			case 3:
				return String("payload ") + itos(next() % 1000);
			case 4:
				return Vector3(real_t(next() % 100), 2.0, -3.5);
			case 5: {
				Array array;
				array.push_back(1);
				array.push_back("a");
				array.push_back(2.5);
				return array;
			}
			case 6: {
				Dictionary dictionary;
				Array nested;
				nested.push_back(1);
				nested.push_back(Vector2(1, 2));
				dictionary["k"] = nested;
				dictionary[3] = String("v");
				return dictionary;
			}
			case 7: {
				PackedByteArray bytes;
				bytes.resize(range(0, 300));
				return bytes;
			}
			case 8: {
				PackedFloat32Array floats;
				floats.resize(range(0, 40));
				return floats;
			}
			case 9: {
				Array deep;
				Array current = deep;
				for (int i = 0; i < 12; i++) {
					Array inner;
					current.push_back(inner);
					current = inner;
				}
				return deep;
			}
			case 10:
				return StringName("name");
			case 11:
				return Color(1, 0.5, 0.25);
			case 12:
				return Transform3D();
			default:
				return true;
		}
	}
};

// A body with one variable of every kind of codec.
class FuzzBody : public TickSyncObject {
public:
	String path;
	int controller = 1;
	bool doll = false;
	TickSchema schema;
	Vector2 position;
	double speed = 0.0;
	int64_t counter = 0;
	Quaternion rotation;
	bool flag = false;
	Vector3 direction = Vector3(1, 0, 0);
	Variant blob;
	int ticks_collected = 0;
	int process_calls = 0;
	int events = 0;
	int authority_changes = 0;

	FuzzBody(const String &p_path, int p_controller, bool p_doll) :
			path(p_path), controller(p_controller), doll(p_doll) {
		schema.add("position", TickCodec::vector2(TickCodec::PRECISION_HALF));
		schema.add("speed", TickCodec::real_ranged(-10.0, 10.0, 12));
		schema.add("counter", TickCodec::integer(16));
		schema.add("rotation", TickCodec::quaternion(10));
		schema.add("flag", TickCodec::boolean());
		schema.add("direction", TickCodec::normalized_vector3(TickCodec::PRECISION_HALF));
		schema.add("blob", TickCodec::variant());
	}

	virtual String get_sync_path() const override { return path; }
	virtual int get_controller_peer() const override { return controller; }
	virtual const TickSchema &get_sync_schema() const override { return schema; }
	virtual bool is_doll_enabled() const override { return doll; }

	virtual Variant get_sync_var(int p_index) const override {
		switch (p_index) {
			case 0:
				return position;
			case 1:
				return speed;
			case 2:
				return counter;
			case 3:
				return rotation;
			case 4:
				return flag;
			case 5:
				return direction;
			default:
				return blob;
		}
	}

	virtual void set_sync_var(int p_index, const Variant &p_value) override {
		switch (p_index) {
			case 0:
				position = p_value;
				break;
			case 1:
				speed = p_value;
				break;
			case 2:
				counter = p_value;
				break;
			case 3:
				rotation = p_value;
				break;
			case 4:
				flag = p_value;
				break;
			case 5:
				direction = p_value;
				break;
			default:
				blob = p_value;
				break;
		}
	}

	virtual void collect_input(TickDataBuffer &r_input) override {
		const int tick = ticks_collected++;
		r_input.add_int_bits(((tick / 20) % 3) - 1, 2);
		r_input.add_bool((tick / 45) % 2 == 0);
		(void)r_input.add_vector2(Vector2(real_t(tick % 7), 1.0), TickDataBuffer::COMPRESSION_LEVEL_2);
	}

	virtual void process_tick(double p_delta, TickDataBuffer &p_input) override {
		process_calls++;
		int move = 0;
		bool pressed = false;
		if (p_input.size() > 0) {
			move = int(p_input.read_int_bits(2));
			pressed = p_input.read_bool();
			(void)p_input.read_vector2(TickDataBuffer::COMPRESSION_LEVEL_2);
			if (p_input.is_buffer_failed()) {
				move = 0;
				pressed = false;
			}
		}
		position.x += real_t(move) * 5.0f * real_t(p_delta);
		speed = double(move) * 2.5;
		counter = (counter + 1) % 30000;
		flag = pressed;
		rotation = Quaternion(Vector3(0, 1, 0), real_t(counter % 628) * 0.01f);
		direction = move < 0 ? Vector3(-1, 0, 0) : Vector3(1, 0, 0);
		blob = counter / 60;
	}

	virtual void on_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) override { events++; }
	virtual void on_authority_changed(int p_old_owner, int p_new_owner) override { authority_changes++; }
};

class FuzzListener : public TickEngine::Listener {
public:
	int spawns = 0;
	int despawns = 0;
	int events = 0;
	int rejected = 0;

	virtual void on_spawn(const String &p_spawner, uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data) override { spawns++; }
	virtual void on_despawn(const String &p_spawner, uint32_t p_spawn_id) override { despawns++; }
	virtual void on_network_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) override { events++; }
	virtual void on_rejected(const String &p_reason) override { rejected++; }
};

// What an impostor does in one step: sends again what the peer it replaced had sent, mutated, and garbage.
struct Impostor {
	Ref<TickLocalTransport> transport;
	// Copies: the corpus goes on growing with what the honest peers send.
	LocalVector<Sample> samples;
	int sent = 0;

	void take(const Ref<TickLocalTransport> &p_transport, const LocalVector<Sample> &p_corpus) {
		transport = p_transport;
		const int id = p_transport->get_local_peer_id();
		for (const Sample &sample : p_corpus) {
			if (sample.from == id) {
				samples.push_back(sample);
			}
		}
	}

	void act(Fuzzer &r_fuzzer, uint32_t p_frame, int p_max_packets) {
		// What the others sent to this peer isn't read: dropped, so the test doesn't grow.
		TickTransport::Packet packet;
		while (transport->pop_packet(packet)) {
		}
		TickTransport::Event event;
		while (transport->pop_event(event)) {
		}
		LocalVector<int> peers;
		transport->get_connected_peers(peers);
		if (peers.is_empty()) {
			return;
		}
		const int count = r_fuzzer.range(0, p_max_packets);
		for (int i = 0; i < count; i++) {
			LocalVector<uint8_t> bytes;
			int target = peers[r_fuzzer.next() % peers.size()];
			int channel = r_fuzzer.range(0, 4);
			TickTransport::TransferMode mode = TickTransport::TransferMode(r_fuzzer.range(0, 2));
			const int kind = r_fuzzer.range(0, 99);
			if (kind < 85 && !samples.is_empty()) {
				const Sample &sample = samples[r_fuzzer.next() % samples.size()];
				bytes = sample.data;
				if (kind < 70) {
					r_fuzzer.mutate(bytes, p_frame);
				}
				if (r_fuzzer.chance(85)) {
					channel = sample.channel;
					mode = sample.mode;
				}
				if (r_fuzzer.chance(70) && peers.has(sample.to)) {
					target = sample.to;
				}
			} else {
				r_fuzzer.random_packet(bytes);
			}
			if (bytes.is_empty()) {
				continue;
			}
			transport->send(target, channel, mode, bytes.ptr(), int(bytes.size()));
			sent++;
		}
	}
};

// Three peers with a single authority (peer 1), in a star or in a mesh. Every peer has a body per peer (dolls on the
// others in a mesh) and an object the authority registers and removes along the way.
struct SyncWorld {
	TickLocalNetwork network;
	Ref<TickLocalTransport> raw[4];
	Ref<TapTransport> taps[4];
	TickSyncCore cores[4];
	FuzzListener listeners[4];
	FuzzBody *bodies[4][4] = {};
	FuzzBody *extras[4] = {};
	bool extra_registered = false;
	uint32_t live_spawn = 0;
	LocalVector<Sample> corpus;
	bool live[4] = { false, true, true, true };
	bool mesh = false;
	int steps = 0;
	uint64_t slowest_usec = 0;

	SyncWorld(bool p_mesh, bool p_trusted, uint64_t p_seed) {
		mesh = p_mesh;
		network.set_seed(p_seed);
		network.set_latency_usec(15000);
		network.set_jitter_usec(4000);
		network.set_packet_loss(0.01);
		TickEngine::Settings settings;
		settings.trusted = p_trusted;
		for (int peer = 1; peer <= 3; peer++) {
			raw[peer] = network.add_peer();
			taps[peer].instantiate();
			taps[peer]->inner = raw[peer];
			taps[peer]->corpus = &corpus;
			cores[peer].set_settings(settings);
			cores[peer].set_listener(&listeners[peer]);
			for (int controller = 1; controller <= 3; controller++) {
				bodies[peer][controller] = memnew(FuzzBody(vformat("body_%d", controller), controller, p_mesh));
				cores[peer].register_object(bodies[peer][controller]);
			}
			extras[peer] = memnew(FuzzBody("extra", 2, false));
			if (peer != 1) {
				cores[peer].register_object(extras[peer]);
			}
			REQUIRE(cores[peer].start(taps[peer], network.get_time_usec()) == OK);
		}
		if (p_mesh) {
			network.connect_all();
		} else {
			network.connect_peers(1, 2);
			network.connect_peers(1, 3);
		}
	}

	~SyncWorld() {
		for (int peer = 1; peer <= 3; peer++) {
			cores[peer].stop();
			for (int controller = 1; controller <= 3; controller++) {
				memdelete(bodies[peer][controller]);
			}
			memdelete(extras[peer]);
		}
	}

	// The honest peers play: events both ways, spawns, an object that comes and goes, interest.
	void activity(Fuzzer &r_fuzzer) {
		const int tick = steps;
		for (int peer = 2; peer <= 3; peer++) {
			if (!live[peer] || !cores[peer].is_welcomed()) {
				continue;
			}
			if (tick % 7 == peer && cores[peer].get_net_id(bodies[peer][peer]) != 0) {
				cores[peer].send_event(bodies[peer][peer], "poke", r_fuzzer.random_payload(), TICK_FRAME_NONE, 0);
			}
			if (tick % 13 == peer) {
				cores[peer].send_event(nullptr, "say", r_fuzzer.random_payload(), TICK_FRAME_NONE, 0);
			}
		}
		if (!live[1]) {
			return;
		}
		TickSyncCore &server = cores[1];
		if (tick % 11 == 0) {
			server.send_event(bodies[1][2], "hit", r_fuzzer.random_payload(), tick % 22 == 0 ? server.get_event_frame(0.2) : TICK_FRAME_NONE, 0);
		}
		if (tick % 17 == 0) {
			server.send_event(nullptr, "round", r_fuzzer.random_payload(), TICK_FRAME_NONE, 0);
		}
		if (tick % 120 == 30) {
			live_spawn = server.spawn("Spawner", 0, vformat("Thing_%d", tick), 2, r_fuzzer.random_payload());
		} else if (tick % 120 == 90 && live_spawn != 0) {
			server.despawn(live_spawn);
			live_spawn = 0;
		}
		if (tick % 200 == 100 && !extra_registered) {
			server.register_object(extras[1]);
			extra_registered = true;
		} else if (tick % 200 == 160 && extra_registered) {
			server.unregister_object(extras[1]);
			extra_registered = false;
		}
		if (tick % 50 == 25) {
			server.set_relevant(bodies[1][1], 2, (tick / 50) % 2 == 0);
		}
	}

	void step() {
		network.process(1.0 / 60.0);
		const uint64_t now = network.get_time_usec();
		for (int peer = 1; peer <= 3; peer++) {
			if (live[peer] && cores[peer].is_running()) {
				const uint64_t before = OS::get_singleton()->get_ticks_usec();
				cores[peer].process(1.0 / 60.0, now);
				cores[peer].update_interpolation(now);
				slowest_usec = MAX(slowest_usec, OS::get_singleton()->get_ticks_usec() - before);
			}
		}
		steps++;
	}

	// The engine of a peer stops, and an impostor keeps its place in the network.
	void take_over(int p_peer, Impostor &r_impostor) {
		cores[p_peer].stop();
		live[p_peer] = false;
		r_impostor.take(raw[p_peer], corpus);
	}
};

static void run_sync_campaign(bool p_mesh, bool p_trusted, int p_impostor, uint64_t p_seed, int p_steps) {
	Fuzzer fuzzer(p_seed);
	SyncWorld world(p_mesh, p_trusted, p_seed);
	for (int i = 0; i < 300; i++) {
		world.activity(fuzzer);
		world.step();
	}
	REQUIRE(world.cores[2].is_predicting());
	REQUIRE(world.cores[3].is_predicting());
	Impostor impostor;
	world.take_over(p_impostor, impostor);
	REQUIRE(impostor.samples.size() > 50);
	const uint64_t memory_before = Memory::get_mem_usage();
	const uint64_t rewinds_before = world.cores[2].get_stats().rewinds;
	const uint32_t frame_before = world.cores[p_impostor == 1 ? 2 : 1].get_frame();

	for (int i = 0; i < p_steps; i++) {
		world.activity(fuzzer);
		impostor.act(fuzzer, world.cores[p_impostor == 1 ? 2 : 1].get_frame(), 6);
		world.step();
		if (p_impostor != 1 && i % 60 == 59) {
			// The server and the honest client go on, in sync: the impostor's packets only move its own body.
			REQUIRE(world.cores[1].is_running());
			REQUIRE(world.cores[2].is_running());
			CHECK(world.cores[2].is_predicting());
			const real_t apart = Math::abs(world.bodies[1][2]->position.x - world.bodies[2][2]->position.x);
			CHECK_MESSAGE(apart < 2.5, vformat("Seed %d, step %d: the honest client's body is %f apart from the server's.", p_seed, i, apart));
		}
	}
	const uint64_t memory_after = Memory::get_mem_usage();
	CHECK(impostor.sent > p_steps);
	if (p_impostor != 1) {
		// The server kept its pace: one frame per step, with any hitch made up for.
		const int32_t advanced = int32_t(world.cores[1].get_frame() - frame_before);
		CHECK_MESSAGE(Math::abs(advanced - p_steps) <= 8, vformat("Seed %d: the server advanced %d frames in %d steps.", p_seed, advanced, p_steps));
		CHECK(world.cores[1].get_stats().malformed_packets > 0);
		// The impostor's packets don't make the honest client mispredict.
		const uint64_t rewinds = world.cores[2].get_stats().rewinds - rewinds_before;
		CHECK_MESSAGE(rewinds < uint64_t(p_steps / 20 + 10), vformat("Seed %d: %d rewinds on the honest client.", p_seed, rewinds));
	} else {
		CHECK(world.cores[2].is_running());
		CHECK(world.cores[3].is_running());
	}
	CHECK_MESSAGE(world.slowest_usec < 2000000, vformat("Seed %d: a step took %d ms.", p_seed, world.slowest_usec / 1000));
	const int64_t grown = int64_t(memory_after) - int64_t(memory_before);
	CHECK_MESSAGE(grown < 96 * 1024 * 1024, vformat("Seed %d: memory grew %d KiB during the campaign.", p_seed, grown / 1024));
	MESSAGE(vformat("seed %d: %d packets from the impostor, slowest step %d us, memory %+d KiB, malformed on 1/2/3: %d/%d/%d", p_seed, impostor.sent, world.slowest_usec, grown / 1024, world.cores[1].get_stats().malformed_packets, world.cores[2].get_stats().malformed_packets, world.cores[3].get_stats().malformed_packets));
}

static void run_sync_campaigns(bool p_mesh, bool p_trusted, int p_impostor) {
	const int seeds = fuzz_env("TICK_FUZZ_SEEDS", 3);
	const int first = fuzz_env("TICK_FUZZ_SEED", 1);
	const int steps = fuzz_env("TICK_FUZZ_STEPS", 600);
	for (int seed = first; seed < first + seeds; seed++) {
		run_sync_campaign(p_mesh, p_trusted, p_impostor, uint64_t(seed), steps);
	}
}

TEST_CASE("[Modules][TickSyncFuzz] Star: a hostile client against the server") {
	ERR_PRINT_OFF;
	run_sync_campaigns(false, false, 3);
	ERR_PRINT_ON;
}

TEST_CASE("[Modules][TickSyncFuzz] Star: a hostile server against its clients") {
	ERR_PRINT_OFF;
	run_sync_campaigns(false, false, 1);
	ERR_PRINT_ON;
}

TEST_CASE("[Modules][TickSyncFuzz] Mesh with a host: a hostile player against the host and the other player") {
	ERR_PRINT_OFF;
	run_sync_campaigns(true, false, 3);
	ERR_PRINT_ON;
}

TEST_CASE("[Modules][TickSyncFuzz] Mesh with a host: a hostile host against the players") {
	ERR_PRINT_OFF;
	run_sync_campaigns(true, false, 1);
	ERR_PRINT_ON;
}

TEST_CASE("[Modules][TickSyncFuzz] Trusted star: a hostile client against the server") {
	ERR_PRINT_OFF;
	run_sync_campaigns(false, true, 3);
	ERR_PRINT_ON;
}

// ------------------------------------------------------------------------------------------------ Distributed mesh

// Honest nodes of a distributed mesh under chaos: links that drop and come back, nodes that restart, roles moved by
// hand, ownership changing all the time. Once the mesh is whole again, every node must end with the same view.
struct ChaosMesh {
	static constexpr int MAX_NODES = 5;
	static constexpr int OBJECTS = 4;

	TickLocalNetwork network;
	Ref<TickLocalTransport> transports[MAX_NODES + 1];
	Ref<TapTransport> taps[MAX_NODES + 1];
	LocalVector<Sample> corpus;
	TickMeshCore *cores[MAX_NODES + 1] = {};
	FuzzListener listeners[MAX_NODES + 1];
	FuzzBody *bodies[MAX_NODES + 1][OBJECTS] = {};
	bool linked[MAX_NODES + 1][MAX_NODES + 1] = {};
	// Steps until a node that is down starts again (0: it's up).
	int down[MAX_NODES + 1] = {};
	int count = 0;
	int restarts = 0;
	int cuts = 0;
	int role_moves = 0;
	int transfers = 0;
	TickEngine::Settings settings;
	uint64_t slowest_usec = 0;

	ChaosMesh(int p_nodes, const Vector<int> &p_candidates, int p_quorum, uint64_t p_seed) {
		count = p_nodes;
		network.set_seed(p_seed);
		network.set_latency_usec(10000);
		network.set_jitter_usec(3000);
		settings.trusted = true;
		settings.interpolate_remote = false;
		settings.role_candidates = p_candidates;
		settings.role_quorum = p_quorum;
		for (int i = 1; i <= count; i++) {
			transports[i] = network.add_peer();
			taps[i].instantiate();
			taps[i]->inner = transports[i];
			taps[i]->corpus = &corpus;
		}
		for (int i = 1; i <= count; i++) {
			start_node(i);
		}
		for (int a = 1; a <= count; a++) {
			for (int b = a + 1; b <= count; b++) {
				set_link(a, b, true);
			}
		}
	}

	~ChaosMesh() {
		for (int i = 1; i <= count; i++) {
			stop_node(i);
		}
	}

	void start_node(int p_node) {
		cores[p_node] = memnew(TickMeshCore);
		cores[p_node]->set_settings(settings);
		cores[p_node]->set_listener(&listeners[p_node]);
		for (int k = 0; k < OBJECTS; k++) {
			bodies[p_node][k] = memnew(FuzzBody(vformat("obj_%d", k), (k % count) + 1, false));
			cores[p_node]->register_object(bodies[p_node][k]);
		}
		REQUIRE(cores[p_node]->start(taps[p_node], network.get_time_usec()) == OK);
	}

	void stop_node(int p_node) {
		if (cores[p_node] == nullptr) {
			return;
		}
		cores[p_node]->stop();
		for (int k = 0; k < OBJECTS; k++) {
			cores[p_node]->unregister_object(bodies[p_node][k]);
			memdelete(bodies[p_node][k]);
			bodies[p_node][k] = nullptr;
		}
		memdelete(cores[p_node]);
		cores[p_node] = nullptr;
	}

	void set_link(int p_a, int p_b, bool p_linked) {
		if (linked[p_a][p_b] == p_linked) {
			return;
		}
		linked[p_a][p_b] = p_linked;
		linked[p_b][p_a] = p_linked;
		if (p_linked) {
			network.connect_peers(p_a, p_b);
		} else {
			network.disconnect_peers(p_a, p_b);
		}
	}

	// The process of a node dies: its links drop, and it starts again, with nothing, some steps later.
	void crash(int p_node, int p_steps_down) {
		if (down[p_node] > 0) {
			return;
		}
		for (int other = 1; other <= count; other++) {
			if (other != p_node) {
				set_link(p_node, other, false);
			}
		}
		stop_node(p_node);
		// What was on the way to it is gone with its process.
		TickTransport::Packet packet;
		while (transports[p_node]->pop_packet(packet)) {
		}
		TickTransport::Event event;
		while (transports[p_node]->pop_event(event)) {
		}
		down[p_node] = p_steps_down;
		restarts++;
	}

	void revive(int p_node) {
		TickTransport::Packet packet;
		while (transports[p_node]->pop_packet(packet)) {
		}
		TickTransport::Event event;
		while (transports[p_node]->pop_event(event)) {
		}
		start_node(p_node);
		for (int other = 1; other <= count; other++) {
			if (other != p_node && down[other] == 0) {
				set_link(p_node, other, true);
			}
		}
	}

	// `p_gentle`: only what the game does (roles moved by hand, ownership); no link drops, no restarts.
	void chaos(Fuzzer &r_fuzzer, bool p_gentle = false) {
		const int a = r_fuzzer.range(1, count);
		const int b = r_fuzzer.range(1, count);
		const int roll = r_fuzzer.range(0, 999);
		if (p_gentle && roll < 54) {
			return;
		}
		if (roll < 12) {
			if (a != b && down[a] == 0 && down[b] == 0 && linked[a][b]) {
				set_link(a, b, false);
				cuts++;
			}
		} else if (roll < 50) {
			if (a != b && down[a] == 0 && down[b] == 0) {
				set_link(a, b, true);
			}
		} else if (roll < 54) {
			crash(a, r_fuzzer.range(5, 240));
		} else if (roll < 62) {
			if (cores[a] && cores[b] && cores[a]->is_running()) {
				const int c = r_fuzzer.range(1, count);
				ERR_PRINT_OFF;
				if (cores[a]->change_roles(b, c) == OK) {
					role_moves++;
				}
				ERR_PRINT_ON;
			}
		} else if (roll < 100) {
			if (cores[a] && cores[a]->is_running()) {
				FuzzBody *body = bodies[a][r_fuzzer.range(0, OBJECTS - 1)];
				ERR_PRINT_OFF;
				Error err = ERR_UNAVAILABLE;
				switch (r_fuzzer.range(0, 2)) {
					case 0:
						err = cores[a]->request_authority(body);
						break;
					case 1:
						err = cores[a]->release_authority(body, r_fuzzer.range(0, count));
						break;
					default:
						err = cores[a]->assign_authority(body, r_fuzzer.range(0, count));
						break;
				}
				ERR_PRINT_ON;
				transfers += err == OK ? 1 : 0;
			}
		}
	}

	void step() {
		network.process(1.0 / 60.0);
		const uint64_t now = network.get_time_usec();
		for (int i = 1; i <= count; i++) {
			if (down[i] > 0) {
				down[i]--;
				if (down[i] == 0) {
					revive(i);
				}
				continue;
			}
			if (cores[i] && cores[i]->is_running()) {
				const uint64_t before = OS::get_singleton()->get_ticks_usec();
				cores[i]->process(1.0 / 60.0, now);
				slowest_usec = MAX(slowest_usec, OS::get_singleton()->get_ticks_usec() - before);
			}
		}
	}

	void heal() {
		for (int i = 1; i <= count; i++) {
			if (down[i] > 0) {
				down[i] = 0;
				revive(i);
			}
		}
		for (int a = 1; a <= count; a++) {
			for (int b = a + 1; b <= count; b++) {
				set_link(a, b, true);
			}
		}
	}

	void run(int p_steps) {
		for (int i = 0; i < p_steps; i++) {
			step();
		}
	}

	// A line about what a node knows, for the failure messages.
	String describe(int p_node) const {
		const TickMeshCore &core = *cores[p_node];
		String text = vformat("node %d: term %d registry %d clock %d frame %d;", p_node, core.get_roles_term(), core.get_settings().registry_peer, core.get_settings().clock_master, core.get_frame());
		for (int k = 0; k < OBJECTS; k++) {
			text += vformat(" obj_%d id %d owner %d v%d;", k, core.get_net_id(bodies[p_node][k]), core.get_owner(bodies[p_node][k]), core.get_version(bodies[p_node][k]));
		}
		return text;
	}
};

// The nodes (but `p_skip`) agree on the roles and on every object, the registry answers, and the owners simulate.
static void check_mesh(ChaosMesh &r_mesh, const String &p_what, int p_skip = 0) {
	String state;
	for (int i = 1; i <= r_mesh.count; i++) {
		if (i != p_skip) {
			state += "\n  " + r_mesh.describe(i);
		}
	}
	const String context = vformat("%s (%d cuts, %d restarts, %d role moves, %d transfers):%s", p_what, r_mesh.cuts, r_mesh.restarts, r_mesh.role_moves, r_mesh.transfers, state);
	const int reference = p_skip == 1 ? 2 : 1;
	const TickMeshCore &first = *r_mesh.cores[reference];
	bool same_roles = true;
	bool same_objects = true;
	bool unique_ids = true;
	bool registered = true;
	bool no_malformed = true;
	bool same_frames = true;
	for (int i = 1; i <= r_mesh.count; i++) {
		if (i == p_skip) {
			continue;
		}
		const TickMeshCore &core = *r_mesh.cores[i];
		same_roles = same_roles && core.get_roles_term() == first.get_roles_term() && core.get_settings().registry_peer == first.get_settings().registry_peer && core.get_settings().clock_master == first.get_settings().clock_master;
		no_malformed = no_malformed && core.get_stats().malformed_packets == 0;
		const int64_t apart = int64_t(core.get_frame()) - int64_t(first.get_frame());
		same_frames = same_frames && apart >= -4 && apart <= 4;
		for (int k = 0; k < ChaosMesh::OBJECTS; k++) {
			const uint16_t id = core.get_net_id(r_mesh.bodies[i][k]);
			registered = registered && id != 0;
			same_objects = same_objects && id == first.get_net_id(r_mesh.bodies[reference][k]) && core.get_owner(r_mesh.bodies[i][k]) == first.get_owner(r_mesh.bodies[reference][k]) && core.get_version(r_mesh.bodies[i][k]) == first.get_version(r_mesh.bodies[reference][k]);
			for (int other = k + 1; other < ChaosMesh::OBJECTS; other++) {
				unique_ids = unique_ids && (id == 0 || id != core.get_net_id(r_mesh.bodies[i][other]));
			}
		}
	}
	CHECK_MESSAGE(same_roles, (String("The nodes disagree on the roles. ") + context));
	CHECK_MESSAGE(registered, (String("An object has no net id on a node. ") + context));
	CHECK_MESSAGE(same_objects, (String("The nodes disagree on an object. ") + context));
	CHECK_MESSAGE(unique_ids, (String("Two objects share a net id. ") + context));
	if (p_skip == 0 && !no_malformed) {
		// Not a failure: messages that arrive for a role the node just lost (or from a link that just came back) are
		// counted as malformed. Reported, since the counter then says "malformed" about packets of honest nodes.
		String counts;
		for (int i = 1; i <= r_mesh.count; i++) {
			counts += vformat(" node %d: %d;", i, r_mesh.cores[i]->get_stats().malformed_packets);
		}
		MESSAGE(vformat("%s: malformed packets counted between honest nodes:%s", p_what, counts));
	}
	CHECK_MESSAGE(same_frames, (String("The nodes' frames are apart. ") + context));
	CHECK_MESSAGE(r_mesh.slowest_usec < 2000000, (vformat("A step took %d ms. ", r_mesh.slowest_usec / 1000) + context));
	if (!same_roles || !registered || !same_objects) {
		return;
	}

	// The registry answers: an object changes owner through it, on every node.
	const int registry = first.get_settings().registry_peer;
	int asker = 0;
	for (int i = 1; i <= r_mesh.count && asker == 0; i++) {
		if (i != p_skip && i != registry) {
			asker = i;
		}
	}
	int index = 0;
	if (r_mesh.cores[asker]->get_owner(r_mesh.bodies[asker][0]) == asker) {
		index = 1;
	}
	ERR_PRINT_OFF;
	const Error asked = r_mesh.cores[asker]->request_authority(r_mesh.bodies[asker][index]);
	ERR_PRINT_ON;
	r_mesh.run(180);
	bool answered = asked == OK;
	for (int i = 1; i <= r_mesh.count && answered; i++) {
		answered = i == p_skip || r_mesh.cores[i]->get_owner(r_mesh.bodies[i][index]) == asker;
	}
	String after;
	for (int i = 1; i <= r_mesh.count; i++) {
		if (i != p_skip) {
			after += "\n  " + r_mesh.describe(i);
		}
	}
	CHECK_MESSAGE(answered, (vformat("Node %d asked for obj_%d (error %d) and didn't get it on every node. ", asker, index, int(asked)) + context + "\n after:" + after));

	// Every owned object moves on its owner, and the other nodes follow it.
	int64_t before[ChaosMesh::OBJECTS] = {};
	for (int k = 0; k < ChaosMesh::OBJECTS; k++) {
		const int owner = first.get_owner(r_mesh.bodies[reference][k]);
		before[k] = owner > 0 && owner != p_skip ? r_mesh.bodies[owner][k]->counter : 0;
	}
	r_mesh.run(120);
	bool simulated = true;
	bool followed = true;
	String lagging;
	for (int k = 0; k < ChaosMesh::OBJECTS; k++) {
		const int owner = first.get_owner(r_mesh.bodies[reference][k]);
		if (owner <= 0 || owner == p_skip) {
			continue;
		}
		if (r_mesh.bodies[owner][k]->counter == before[k]) {
			simulated = false;
			lagging += vformat("\n  obj_%d isn't simulated by its owner, node %d (counter %d)", k, owner, before[k]);
		}
		for (int i = 1; i <= r_mesh.count; i++) {
			if (i == p_skip) {
				continue;
			}
			const int64_t apart = (r_mesh.bodies[i][k]->counter - r_mesh.bodies[owner][k]->counter + 30000) % 30000;
			if (!(apart <= 30 || apart >= 30000 - 30)) {
				followed = false;
				lagging += vformat("\n  obj_%d on node %d: counter %d, on its owner (node %d) %d; stale states on node %d: %d", k, i, r_mesh.bodies[i][k]->counter, owner, r_mesh.bodies[owner][k]->counter, i, r_mesh.cores[i]->get_stats().stale_states);
			}
		}
	}
	CHECK_MESSAGE(simulated, (String("An owner doesn't simulate its object. ") + context + lagging));
	CHECK_MESSAGE(followed, (String("A node doesn't follow an object's owner. ") + context + lagging));
}

static void run_chaos(int p_nodes, const Vector<int> &p_candidates, int p_quorum, uint64_t p_seed, int p_steps) {
	Fuzzer fuzzer(p_seed);
	ChaosMesh mesh(p_nodes, p_candidates, p_quorum, p_seed);
	mesh.run(120);
	for (int i = 0; i < p_steps; i++) {
		mesh.chaos(fuzzer);
		mesh.step();
	}
	// The mesh is whole again, and settles.
	mesh.heal();
	mesh.run(600);
	check_mesh(mesh, vformat("Seed %d, %d nodes, %d candidates, quorum %d", p_seed, p_nodes, p_candidates.size(), p_quorum));
}

// A node of the mesh turns hostile: an impostor takes its place and sends what it had sent, mutated. Then it leaves,
// and the other nodes must agree again.
static void run_mesh_impostor(int p_impostor, uint64_t p_seed, int p_steps) {
	Fuzzer fuzzer(p_seed);
	ChaosMesh mesh(4, Vector<int>(), 0, p_seed);
	for (int i = 0; i < 360; i++) {
		mesh.chaos(fuzzer, true);
		mesh.step();
	}
	Impostor impostor;
	mesh.stop_node(p_impostor);
	impostor.take(mesh.transports[p_impostor], mesh.corpus);
	REQUIRE(impostor.samples.size() > 50);
	const uint64_t memory_before = Memory::get_mem_usage();
	for (int i = 0; i < p_steps; i++) {
		mesh.chaos(fuzzer, true);
		impostor.act(fuzzer, mesh.cores[p_impostor == 1 ? 2 : 1]->get_frame(), 5);
		ERR_PRINT_OFF;
		mesh.step();
		ERR_PRINT_ON;
	}
	const int64_t grown = int64_t(Memory::get_mem_usage()) - int64_t(memory_before);
	CHECK_MESSAGE(grown < 96 * 1024 * 1024, vformat("Seed %d: memory grew %d KiB during the campaign.", p_seed, grown / 1024));
	MESSAGE(vformat("seed %d: %d packets from the impostor (node %d), slowest step %d us, memory %+d KiB", p_seed, impostor.sent, p_impostor, mesh.slowest_usec, grown / 1024));
	// The hostile node is gone.
	for (int other = 1; other <= mesh.count; other++) {
		if (other != p_impostor) {
			mesh.set_link(p_impostor, other, false);
		}
	}
	ERR_PRINT_OFF;
	mesh.run(600);
	ERR_PRINT_ON;
	check_mesh(mesh, vformat("Seed %d, after the hostile node %d left", p_seed, p_impostor), p_impostor);
}

static void run_chaos_seeds(int p_nodes, const Vector<int> &p_candidates, int p_quorum) {
	const int seeds = fuzz_env("TICK_FUZZ_SEEDS", 3);
	const int first = fuzz_env("TICK_FUZZ_SEED", 1);
	const int steps = fuzz_env("TICK_FUZZ_STEPS", 600);
	for (int seed = first; seed < first + seeds; seed++) {
		run_chaos(p_nodes, p_candidates, p_quorum, uint64_t(seed), steps);
	}
}

TEST_CASE("[Modules][TickSyncFuzz] Distributed mesh under chaos: any node takes the roles") {
	run_chaos_seeds(4, Vector<int>(), 0);
}

TEST_CASE("[Modules][TickSyncFuzz] Distributed mesh under chaos: two candidates") {
	Vector<int> candidates;
	candidates.push_back(1);
	candidates.push_back(2);
	run_chaos_seeds(4, candidates, 0);
}

TEST_CASE("[Modules][TickSyncFuzz] Distributed mesh under chaos: a quorum of three in five") {
	run_chaos_seeds(5, Vector<int>(), 3);
}

TEST_CASE("[Modules][TickSyncFuzz] Distributed mesh under chaos: candidates and a quorum") {
	Vector<int> candidates;
	candidates.push_back(2);
	candidates.push_back(4);
	candidates.push_back(1);
	run_chaos_seeds(5, candidates, 3);
}

TEST_CASE("[Modules][TickSyncFuzz] Distributed mesh: a hostile node, then the others agree again") {
	const int seeds = fuzz_env("TICK_FUZZ_SEEDS", 3);
	const int first = fuzz_env("TICK_FUZZ_SEED", 1);
	const int steps = fuzz_env("TICK_FUZZ_STEPS", 600);
	for (int seed = first; seed < first + seeds; seed++) {
		run_mesh_impostor(4, uint64_t(seed), steps);
	}
}

TEST_CASE("[Modules][TickSyncFuzz] Distributed mesh: the node with the roles turns hostile, then the others agree again") {
	const int seeds = fuzz_env("TICK_FUZZ_SEEDS", 3);
	const int first = fuzz_env("TICK_FUZZ_SEED", 1);
	const int steps = fuzz_env("TICK_FUZZ_STEPS", 600);
	for (int seed = first; seed < first + seeds; seed++) {
		run_mesh_impostor(1, uint64_t(seed), steps);
	}
}

// ------------------------------------------------------------------------------------------------ Real sockets

// ENet packet flags and event types, as the engine's scripting API has them (the tests can't include ENet's headers:
// the bare sockets below go through that API, like a script would).
static constexpr int ROGUE_FLAG_RELIABLE = 1;
static constexpr int ROGUE_FLAG_UNSEQUENCED = 2;
static constexpr int ROGUE_EVENT_CONNECT = 1;
static constexpr int ROGUE_EVENT_DISCONNECT = 2;
static constexpr int ROGUE_EVENT_RECEIVE = 3;

static void rogue_send(const Ref<RefCounted> &p_link, int p_channel, const uint8_t *p_data, int p_size, int p_flags) {
	if (p_link.is_null() || !bool(p_link->call("is_active"))) {
		return;
	}
	PackedByteArray bytes;
	bytes.resize(p_size);
	if (p_size > 0) {
		memcpy(bytes.ptrw(), p_data, p_size);
	}
	p_link->call("send", p_channel, bytes, p_flags);
}

// A bare ENet endpoint: what a program that isn't this module can do.
struct RogueSocket {
	Ref<RefCounted> socket;
	Ref<RefCounted> link;
	bool connected = false;
	bool disconnected = false;
	uint32_t disconnect_data = 0;
	LocalVector<LocalVector<uint8_t>> control;

	bool open(const String &p_bind, const String &p_address, int p_port, uint32_t p_data, int p_channels) {
		socket = Ref<RefCounted>(Object::cast_to<RefCounted>(ClassDB::instantiate("ENetConnection")));
		if (socket.is_null()) {
			return false;
		}
		// Two peers, like the module's pair sockets: the registration with the host, then the other player.
		if (int(socket->call("create_host_bound", p_bind, 0, 2, p_channels, 0, 0)) != OK) {
			socket.unref();
			return false;
		}
		// The range coder, the module's default.
		socket->call("compress", 1);
		return connect(p_address, p_port, p_data, p_channels);
	}

	bool connect(const String &p_address, int p_port, uint32_t p_data, int p_channels) {
		link = socket->call("connect_to_host", p_address, p_port, p_channels, int(p_data));
		connected = false;
		disconnected = false;
		return link.is_valid();
	}

	void poll() {
		if (socket.is_null()) {
			return;
		}
		for (int i = 0; i < 256; i++) {
			const Array event = socket->call("service", 0);
			const int type = event.size() >= 4 ? int(event[0]) : 0;
			if (type <= 0) {
				break;
			}
			const Ref<RefCounted> peer = event[1];
			if (type == ROGUE_EVENT_CONNECT) {
				connected = true;
			} else if (type == ROGUE_EVENT_DISCONNECT) {
				if (peer == link) {
					connected = false;
					disconnected = true;
					disconnect_data = uint32_t(int64_t(event[2]));
				}
			} else if (type == ROGUE_EVENT_RECEIVE && peer.is_valid()) {
				const PackedByteArray packet = peer->call("get_packet");
				if (int(event[3]) == 0) {
					LocalVector<uint8_t> message;
					message.resize(uint32_t(packet.size()));
					if (packet.size() > 0) {
						memcpy(message.ptr(), packet.ptr(), packet.size());
					}
					control.push_back(message);
				}
			}
		}
		socket->call("flush");
	}

	void send(int p_channel, const uint8_t *p_data, int p_size, int p_flags) {
		rogue_send(link, p_channel, p_data, p_size, p_flags);
	}

	// Leaves at once. The socket goes with the link: a socket serviced after `peer_disconnect_now()` reads freed memory
	// in the engine (see `enet_close_link()`).
	void disconnect_now(uint32_t p_data) {
		if (socket.is_valid()) {
			if (link.is_valid() && bool(link->call("is_active"))) {
				link->call("peer_disconnect_now", int(p_data));
			}
			socket->call("flush");
			socket->call("destroy");
			socket.unref();
			link.unref();
		}
		disconnected = true;
		connected = false;
	}

	void close() {
		if (socket.is_valid()) {
			if (link.is_valid() && bool(link->call("is_active"))) {
				link->call("peer_disconnect_now", 0);
			}
			socket->call("flush");
			socket->call("destroy");
			socket.unref();
			link.unref();
		}
	}
};

static int pick_port(int p_salt) {
	return 45000 + 2 * int((OS::get_singleton()->get_ticks_usec() / 7 + uint64_t(p_salt) * 7919) % 4000);
}

static void fuzz_put_u32(LocalVector<uint8_t> &r_message, uint32_t p_value) {
	for (int i = 0; i < 4; i++) {
		r_message.push_back(uint8_t(p_value >> (8 * i)));
	}
}

// A control message of the hosted mesh: a type and a few fields that look like the real ones, or garbage.
static void random_control(Fuzzer &r_fuzzer, LocalVector<uint8_t> &r_message, int p_port) {
	r_message.clear();
	r_message.push_back(uint8_t(r_fuzzer.chance(90) ? r_fuzzer.range(1, 15) : r_fuzzer.range(0, 255)));
	const int fields = r_fuzzer.range(0, 6);
	for (int i = 0; i < fields; i++) {
		switch (r_fuzzer.range(0, 5)) {
			case 0:
				// A player id.
				fuzz_put_u32(r_message, uint32_t(r_fuzzer.range(0, 12)));
				break;
			case 1:
				fuzz_put_u32(r_message, r_fuzzer.interesting(1000));
				break;
			case 2:
				fuzz_put_u32(r_message, uint32_t(p_port + r_fuzzer.range(-1, 3)));
				break;
			case 3: {
				// A string: an address, most of the time.
				static const char *texts[5] = { "127.0.0.1", "127.0.0.2", "::1", "not an address", "" };
				const String text = texts[r_fuzzer.range(0, 4)];
				const CharString utf8 = text.utf8();
				fuzz_put_u32(r_message, r_fuzzer.chance(90) ? uint32_t(utf8.length()) : r_fuzzer.interesting(16384));
				for (int c = 0; c < utf8.length(); c++) {
					r_message.push_back(uint8_t(utf8[c]));
				}
			} break;
			case 4: {
				const int length = r_fuzzer.range(0, 80);
				fuzz_put_u32(r_message, r_fuzzer.chance(80) ? uint32_t(length) : r_fuzzer.interesting(4096));
				for (int b = 0; b < length; b++) {
					r_message.push_back(uint8_t(r_fuzzer.next()));
				}
			} break;
			default: {
				// A list: a count and ids.
				const int length = r_fuzzer.range(0, 12);
				fuzz_put_u32(r_message, r_fuzzer.chance(80) ? uint32_t(length) : r_fuzzer.interesting(1024));
				for (int b = 0; b < length; b++) {
					fuzz_put_u32(r_message, uint32_t(r_fuzzer.range(0, 12)));
				}
			} break;
		}
	}
	if (r_fuzzer.chance(15)) {
		r_fuzzer.mutate(r_message, 1000);
	}
}

TEST_CASE("[Modules][TickSyncFuzz] Hosted mesh: a hostile player against the host") {
	typedef EnetHostedMeshTransport Mesh;
	static constexpr uint32_t JOIN_MAGIC = 0x544B4D32;
	static constexpr int CHANNELS = 27;
	const int seeds = fuzz_env("TICK_FUZZ_SEEDS", 3);
	const int first = fuzz_env("TICK_FUZZ_SEED", 1);
	const int steps = fuzz_env("TICK_FUZZ_STEPS", 600);
	ERR_PRINT_OFF;
	for (int seed = first; seed < first + seeds; seed++) {
		Fuzzer fuzzer(uint64_t(seed) + 500);
		Ref<Mesh> host;
		int port = 0;
		for (int attempt = 0; attempt < 10 && host.is_null(); attempt++) {
			port = pick_port(attempt + seed);
			host = Mesh::create_host(port, 8);
		}
		REQUIRE(host.is_valid());
		Ref<Mesh> honest[2];
		for (int i = 0; i < 2; i++) {
			honest[i] = Mesh::create_player("127.0.0.1", port);
			for (int t = 0; t < 3000 && honest[i]->get_status() != Mesh::STATUS_CONNECTED; t++) {
				host->poll();
				honest[0]->poll();
				if (i == 1) {
					honest[1]->poll();
				}
				OS::get_singleton()->delay_usec(500);
			}
			REQUIRE(honest[i]->get_status() == Mesh::STATUS_CONNECTED);
		}
		RogueSocket rogue;
		REQUIRE(rogue.open("127.0.0.2", "127.0.0.1", port, JOIN_MAGIC, CHANNELS));
		bool joined = false;
		int sent = 0;
		for (int step = 0; step < steps; step++) {
			host->poll();
			honest[0]->poll();
			honest[1]->poll();
			rogue.poll();
			TickTransport::Packet packet;
			for (int i = 0; i < 2; i++) {
				while (honest[i]->pop_packet(packet)) {
				}
			}
			while (host->pop_packet(packet)) {
			}
			if (rogue.disconnected) {
				// Thrown out, or it left: it joins again.
				rogue.close();
				rogue.open("127.0.0.2", "127.0.0.1", port, JOIN_MAGIC, CHANNELS);
				joined = false;
			}
			if (rogue.connected && !joined) {
				joined = true;
				LocalVector<uint8_t> join;
				join.push_back(11);
				fuzz_put_u32(join, 0);
				if (fuzzer.chance(10)) {
					fuzzer.mutate(join, 0);
				}
				rogue.send(0, join.ptr(), int(join.size()), ROGUE_FLAG_RELIABLE);
			} else if (rogue.connected) {
				const int count = fuzzer.range(0, 4);
				for (int i = 0; i < count; i++) {
					LocalVector<uint8_t> message;
					int channel = 0;
					const int kind = fuzzer.range(0, 9);
					if (kind < 6) {
						random_control(fuzzer, message, port);
					} else if (kind < 8) {
						// A relayed packet: the target first.
						channel = fuzzer.range(14, 26);
						fuzz_put_u32(message, uint32_t(fuzzer.range(0, 6)));
						const int size = fuzzer.chance(5) ? fuzzer.range(2000, 70000) : fuzzer.range(0, 300);
						for (int b = 0; b < size; b++) {
							message.push_back(uint8_t(fuzzer.next()));
						}
					} else {
						channel = fuzzer.range(1, 26);
						fuzzer.random_packet(message);
					}
					static const int flags[3] = { ROGUE_FLAG_RELIABLE, ROGUE_FLAG_UNSEQUENCED, 0 };
					rogue.send(channel, message.ptr(), int(message.size()), flags[fuzzer.range(0, 2)]);
					sent++;
				}
				if (fuzzer.chance(1)) {
					rogue.disconnect_now(fuzzer.interesting(0));
				}
			}
			if (step % 50 == 49) {
				// The honest players still reach each other and the host.
				const uint8_t probe[2] = { 9, 9 };
				CHECK(honest[0]->get_status() == Mesh::STATUS_CONNECTED);
				CHECK(honest[1]->get_status() == Mesh::STATUS_CONNECTED);
				CHECK(honest[0]->is_peer_connected(honest[1]->get_local_peer_id()));
				honest[0]->send(1, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, probe, 2);
			}
			OS::get_singleton()->delay_usec(300);
		}
		CHECK(host->get_status() == Mesh::STATUS_CONNECTED);
		CHECK(host->is_peer_connected(honest[0]->get_local_peer_id()));
		CHECK(host->is_peer_connected(honest[1]->get_local_peer_id()));
		MESSAGE(vformat("seed %d: %d messages from the hostile player; host stats %s", seed, sent, String(Variant(host->get_stats()))));
		rogue.close();
		honest[1]->close();
		honest[0]->close();
		for (int t = 0; t < 30; t++) {
			host->poll();
			OS::get_singleton()->delay_usec(500);
		}
		host->close();
	}
	ERR_PRINT_ON;
}

// Takes what a bare server socket received, and tells which links connected and closed.
static void rogue_server_service(const Ref<RefCounted> &p_server, Fuzzer &r_fuzzer, Ref<RefCounted> &r_host_link, bool p_close_registrations) {
	for (int e = 0; e < 64; e++) {
		const Array event = p_server->call("service", 0);
		const int type = event.size() >= 4 ? int(event[0]) : 0;
		if (type <= 0) {
			break;
		}
		const Ref<RefCounted> peer = event[1];
		if (type == ROGUE_EVENT_CONNECT) {
			if ((uint32_t(int64_t(event[2])) & 0xFFFFFF00) == 0x544B4D00) {
				r_host_link = peer;
			} else if (p_close_registrations && r_fuzzer.chance(80)) {
				// A pair's registration: closed as the real host does, or left open.
				peer->call("peer_disconnect", 0);
			}
		} else if (type == ROGUE_EVENT_DISCONNECT) {
			if (peer == r_host_link) {
				r_host_link.unref();
			}
		} else if (type == ROGUE_EVENT_RECEIVE && peer.is_valid()) {
			peer->call("get_packet");
		}
	}
}

TEST_CASE("[Modules][TickSyncFuzz] Hosted mesh: a hostile host against a player") {
	typedef EnetHostedMeshTransport Mesh;
	static constexpr int CHANNELS = 27;
	const int seeds = fuzz_env("TICK_FUZZ_SEEDS", 3);
	const int first = fuzz_env("TICK_FUZZ_SEED", 1);
	const int steps = fuzz_env("TICK_FUZZ_STEPS", 600);
	ERR_PRINT_OFF;
	for (int seed = first; seed < first + seeds; seed++) {
		Fuzzer fuzzer(uint64_t(seed) + 900);
		// The hostile host: a bare socket that takes the player's connections.
		Ref<RefCounted> server;
		int port = 0;
		for (int attempt = 0; attempt < 10; attempt++) {
			port = pick_port(attempt + seed + 100);
			server = Ref<RefCounted>(Object::cast_to<RefCounted>(ClassDB::instantiate("ENetConnection")));
			if (server.is_valid() && int(server->call("create_host_bound", "127.0.0.1", port, 64, CHANNELS, 0, 0)) == OK) {
				break;
			}
			server.unref();
		}
		REQUIRE(server.is_valid());
		server->call("compress", 1);
		int sent = 0;
		int rounds = 0;
		int step = 0;
		while (step < steps) {
			rounds++;
			Ref<Mesh> player = Mesh::create_player("127.0.0.1", port);
			player->set_host_timeout(0.5);
			player->set_punch_timeout(0.2);
			Ref<RefCounted> host_link;
			bool welcomed = false;
			const int round_steps = fuzzer.range(20, 200);
			for (int i = 0; i < round_steps && step < steps; i++, step++) {
				player->poll();
				TickTransport::Packet packet;
				while (player->pop_packet(packet)) {
				}
				TickTransport::Event player_event;
				while (player->pop_event(player_event)) {
				}
				rogue_server_service(server, fuzzer, host_link, true);
				if (host_link.is_valid() && bool(host_link->call("is_active"))) {
					if (!welcomed) {
						welcomed = true;
						LocalVector<uint8_t> welcome;
						welcome.push_back(1);
						fuzz_put_u32(welcome, uint32_t(fuzzer.range(2, 6)));
						fuzz_put_u32(welcome, fuzzer.chance(70) ? 0 : uint32_t(port + 1));
						fuzz_put_u32(welcome, fuzzer.chance(70) ? 8 : fuzzer.interesting(1024));
						fuzz_put_u32(welcome, fuzzer.chance(70) ? 1 : uint32_t(fuzzer.range(0, 8)));
						if (fuzzer.chance(10)) {
							fuzzer.mutate(welcome, 0);
						}
						rogue_send(host_link, 0, welcome.ptr(), int(welcome.size()), ROGUE_FLAG_RELIABLE);
					}
					const int count = fuzzer.range(0, 3);
					for (int m = 0; m < count; m++) {
						LocalVector<uint8_t> message;
						int channel = 0;
						const int kind = fuzzer.range(0, 9);
						if (kind < 7) {
							random_control(fuzzer, message, port);
						} else if (kind < 9) {
							// Relayed, from some origin.
							channel = fuzzer.range(14, 26);
							fuzz_put_u32(message, uint32_t(fuzzer.range(0, 8)));
							const int size = fuzzer.range(0, 400);
							for (int b = 0; b < size; b++) {
								message.push_back(uint8_t(fuzzer.next()));
							}
						} else {
							channel = fuzzer.range(1, 26);
							fuzzer.random_packet(message);
						}
						rogue_send(host_link, channel, message.ptr(), int(message.size()), fuzzer.chance(70) ? ROGUE_FLAG_RELIABLE : ROGUE_FLAG_UNSEQUENCED);
						sent++;
					}
				}
				server->call("flush");
				OS::get_singleton()->delay_usec(300);
			}
			// The round ends: the host leaves in some way, or the player does.
			if (host_link.is_valid() && bool(host_link->call("is_active")) && fuzzer.chance(70)) {
				static const uint32_t reasons[6] = { 0x544B484F, 0x544B454E, 0x544B5258, 0x544B464C, 0x544B5632, 0 };
				// Gracefully: the server socket goes on being serviced (see `enet_close_link()`).
				host_link->call("peer_disconnect", int(fuzzer.chance(80) ? reasons[fuzzer.range(0, 5)] : fuzzer.next()));
				server->call("flush");
				for (int i = 0; i < 400; i++) {
					player->poll();
					rogue_server_service(server, fuzzer, host_link, false);
					OS::get_singleton()->delay_usec(300);
				}
			}
			player->close();
			// The player's links are gone on this side too.
			for (int i = 0; i < 20; i++) {
				rogue_server_service(server, fuzzer, host_link, false);
				OS::get_singleton()->delay_usec(300);
			}
		}
		MESSAGE(vformat("seed %d: %d messages from the hostile host in %d rounds", seed, sent, rounds));
		server->call("destroy");
	}
	ERR_PRINT_ON;
}

TEST_CASE("[Modules][TickSyncFuzz] Servers' mesh: a hostile connection against a node") {
	static constexpr int CHANNELS = 5;
	const int seeds = fuzz_env("TICK_FUZZ_SEEDS", 3);
	const int first = fuzz_env("TICK_FUZZ_SEED", 1);
	const int steps = fuzz_env("TICK_FUZZ_STEPS", 600);
	ERR_PRINT_OFF;
	for (int seed = first; seed < first + seeds; seed++) {
		Fuzzer fuzzer(uint64_t(seed) + 1300);
		Ref<EnetMeshTransport> node;
		int port = 0;
		for (int attempt = 0; attempt < 10 && node.is_null(); attempt++) {
			port = pick_port(attempt + seed + 200);
			node = EnetMeshTransport::create(1, port);
		}
		REQUIRE(node.is_valid());
		node->add_node(2, "127.0.0.1", port + 1);
		node->add_node(3, "127.0.0.1", port + 2);
		node->set_retry_interval(0.05);
		node->set_node_timeout(0.5);
		TickMeshCore core;
		FuzzBody body("body", 1, false);
		core.register_object(&body);
		TickEngine::Settings settings;
		settings.trusted = true;
		core.set_settings(settings);
		REQUIRE(core.start(node, OS::get_singleton()->get_ticks_usec()) == OK);
		RogueSocket rogue;
		int sent = 0;
		for (int step = 0; step < steps; step++) {
			if (rogue.socket.is_null() || rogue.disconnected) {
				rogue.close();
				// Declares itself some node: one of the list, or not.
				rogue.open("127.0.0.2", "127.0.0.1", port, uint32_t(fuzzer.chance(80) ? fuzzer.range(2, 3) : int(fuzzer.interesting(0))), fuzzer.chance(90) ? CHANNELS : fuzzer.range(1, 30));
			}
			rogue.poll();
			if (rogue.connected) {
				const int count = fuzzer.range(0, 5);
				for (int i = 0; i < count; i++) {
					LocalVector<uint8_t> message;
					fuzzer.random_packet(message);
					static const int flags[3] = { ROGUE_FLAG_RELIABLE, ROGUE_FLAG_UNSEQUENCED, 0 };
					rogue.send(fuzzer.range(0, 4), message.ptr(), int(message.size()), flags[fuzzer.range(0, 2)]);
					sent++;
				}
				if (fuzzer.chance(2)) {
					rogue.disconnect_now(0);
				}
			}
			core.process(1.0 / 60.0, OS::get_singleton()->get_ticks_usec());
			OS::get_singleton()->delay_usec(300);
		}
		CHECK(core.is_running());
		MESSAGE(vformat("seed %d: %d packets from the hostile connection; malformed %d", seed, sent, core.get_stats().malformed_packets));
		rogue.close();
		core.stop();
		core.unregister_object(&body);
	}
	ERR_PRINT_ON;
}

} // namespace TestTickFuzz
