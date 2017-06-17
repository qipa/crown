/*
 * Copyright (c) 2012-2017 Daniele Bartolini and individual contributors.
 * License: https://github.com/taylor001/crown/blob/master/LICENSE
 */

#include "core/containers/array.h"
#include "core/containers/hash_map.h"
#include "core/containers/map.h"
#include "core/error/error.h"
#include "core/filesystem/file.h"
#include "core/guid.h"
#include "core/json/json_object.h"
#include "core/json/sjson.h"
#include "core/math/math.h"
#include "core/memory/temp_allocator.h"
#include "core/strings/dynamic_string.h"
#include "resource/compile_options.h"
#include "resource/expression_language.h"
#include "resource/state_machine_resource.h"
#include "resource/types.h"

namespace crown
{
template <>
struct hash<Guid>
{
	u32 operator()(const Guid& id) const
	{
		return id.data1;
	}
};

namespace state_machine_internal
{
	struct TransitionModeInfo
	{
		const char* name;
		TransitionMode::Enum mode;
	};

	static TransitionModeInfo _transition_mode_map[] =
	{
		{ "immediate",     TransitionMode::IMMEDIATE     },
		{ "animation_end", TransitionMode::ANIMATION_END },
	};
	CE_STATIC_ASSERT(countof(_transition_mode_map) == TransitionMode::COUNT);

	static TransitionMode::Enum name_to_transition_mode(const char* name)
	{
		for (u32 i = 0; i < countof(_transition_mode_map); ++i)
		{
			if (strcmp(name, _transition_mode_map[i].name) == 0)
				return _transition_mode_map[i].mode;
		}

		return TransitionMode::COUNT;
	}

	struct OffsetAccumulator
	{
		u32 _offset;

		OffsetAccumulator()
		{
			_offset = sizeof(StateMachineResource);
		}

		// Returns the offset of
		u32 offset(u32 num_animations, u32 num_transitions)
		{
			const u32 offt = _offset;
			_offset += sizeof(State);
			_offset += sizeof(Transition) * num_transitions;
			_offset += sizeof(AnimationArray);
			_offset += sizeof(Animation) * num_animations;
			return offt;
		}
	};

	struct AnimationInfo
	{
		ALLOCATOR_AWARE;

		StringId64 name;
		DynamicString weight;
		u32 bytecode_entry;

		AnimationInfo(Allocator& a)
			: weight(a)
		{
		}
	};

	struct TransitionInfo
	{
		Transition transition;
		Guid state;
	};

	struct StateInfo
	{
		ALLOCATOR_AWARE;

		Vector<AnimationInfo> animations;
		Vector<TransitionInfo> transitions;
		DynamicString speed;
		u32 speed_bytecode;
		u32 loop;

		StateInfo()
			: animations(default_allocator())
			, transitions(default_allocator())
			, speed(default_allocator())
			, speed_bytecode(UINT32_MAX)
			, loop(0)
		{
		}

		StateInfo(Allocator& a)
			: animations(a)
			, transitions(a)
			, speed(a)
			, speed_bytecode(UINT32_MAX)
			, loop(0)
		{
		}
	};

	struct VariableInfo
	{
		ALLOCATOR_AWARE;

		StringId32 name;
		float value;
		DynamicString name_string;

		VariableInfo()
			: name_string(default_allocator())
		{
		}

		VariableInfo(Allocator& a)
			: name_string(a)
		{
		}
	};

	struct StateMachineCompiler
	{
		CompileOptions _opts;
		Guid _initial_state;
		Map<Guid, StateInfo> _states;
		OffsetAccumulator _offset_accumulator;
		HashMap<Guid, u32> _offsets;
		Vector<VariableInfo> _variables;
		Array<u32> _byte_code;

		StateMachineCompiler(CompileOptions opts)
			: _opts(opts)
			, _states(default_allocator())
			, _offsets(default_allocator())
			, _variables(default_allocator())
			, _byte_code(default_allocator())
		{
		}

		void parse(Buffer& buf)
		{
			TempAllocator4096 ta;
			JsonObject object(ta);
			JsonObject states(ta);
			JsonObject variables(ta);

			sjson::parse(buf, object);
			sjson::parse_object(object["states"], states);
			sjson::parse_object(object["variables"], variables);

			// Parse states
			auto cur = json_object::begin(states);
			auto end = json_object::end(states);
			for (; cur != end; ++cur)
			{
				const FixedString key = cur->pair.first;

				TempAllocator4096 ta;
				JsonObject state(ta);
				JsonObject animations(ta);
				JsonObject transitions(ta);
				sjson::parse_object(states[key], state);
				sjson::parse_object(state["animations"], animations);
				sjson::parse_object(state["transitions"], transitions);

				StateInfo si;

				sjson::parse_string(state["speed"], si.speed);
				si.loop = sjson::parse_bool(state["loop"]);

				// Parse transitions
				{
					auto cur = json_object::begin(transitions);
					auto end = json_object::end(transitions);
					for (; cur != end; ++cur)
					{
						const FixedString key = cur->pair.first;

						JsonObject transition(ta);
						sjson::parse_object(transitions[key], transition);

						DynamicString mode_str(ta);
						sjson::parse_string(transition["mode"], mode_str);
						const u32 mode = name_to_transition_mode(mode_str.c_str());
						DATA_COMPILER_ASSERT(mode != TransitionMode::COUNT
							, _opts
							, "Unknown transition mode: '%s'"
							, mode_str.c_str()
							);

						TransitionInfo ti;
						ti.transition.event        = sjson::parse_string_id(transition["event"]);
						ti.transition.state_offset = UINT32_MAX;
						ti.transition.mode         = mode;
						ti.state                   = sjson::parse_guid(transition["to"]);

						vector::push_back(si.transitions, ti);
					}
				}

				// Parse animations
				{
					auto cur = json_object::begin(animations);
					auto end = json_object::end(animations);
					for (; cur != end; ++cur)
					{
						const FixedString key = cur->pair.first;

						JsonObject animation(ta);
						sjson::parse_object(animations[key], animation);

						DynamicString animation_resource(ta);
						sjson::parse_string(animation["name"], animation_resource);
						DATA_COMPILER_ASSERT_RESOURCE_EXISTS("sprite_animation"
							, animation_resource.c_str()
							, _opts
							);

						AnimationInfo ai(ta);
						ai.name = sjson::parse_resource_id(animation["name"]);
						sjson::parse_string(animation["weight"], ai.weight);

						vector::push_back(si.animations, ai);
					}
					DATA_COMPILER_ASSERT(vector::size(si.animations) > 0
						, _opts
						, "State must contain one animation at least"
						);
				}

				Guid guid = guid::parse(key._data);
				DATA_COMPILER_ASSERT(!map::has(_states, guid)
					, _opts
					, "State GUID duplicated"
					);
				map::set(_states, guid, si);
			}
			DATA_COMPILER_ASSERT(map::size(_states) > 0
				, _opts
				, "State machine must contain one state at least"
				);

			_initial_state = sjson::parse_guid(object["initial_state"]);
			DATA_COMPILER_ASSERT(map::has(_states, _initial_state)
				, _opts
				, "Initial state references non-existing state"
				);

			// Parse variables
			{
				auto cur = json_object::begin(variables);
				auto end = json_object::end(variables);
				for (; cur != end; ++cur)
				{
					JsonObject variable(ta);
					sjson::parse_object(variables[cur->pair.first], variable);

					VariableInfo vi;
					vi.name  = sjson::parse_string_id(variable["name"]);
					vi.value = sjson::parse_float(variable["value"]);
					sjson::parse_string(variable["name"], vi.name_string);

					vector::push_back(_variables, vi);
				}
			}

			// Compute state offsets
			{
				// Limit byte code to 4K
				array::resize(_byte_code, 1024);
				u32 written = 0;

				const u32 num_variables = vector::size(_variables);
				const char** variables = (const char**)default_allocator().allocate(num_variables*sizeof(char*));

				for (u32 i = 0; i < num_variables; ++i)
					variables[i] = _variables[i].name_string.c_str();

				const u32 num_constants = 1;
				const char* constants[] =
				{
					"PI"
				};
				const f32 constant_values[] =
				{
					PI
				};

				auto cur = map::begin(_states);
				auto end = map::end(_states);
				for (; cur != end; ++cur)
				{
					const Guid& guid    = cur->pair.first;
					const StateInfo& si = cur->pair.second;

					const u32 offset = _offset_accumulator.offset(vector::size(si.animations), vector::size(si.transitions));
					hash_map::set(_offsets, guid, offset);

					for (u32 i = 0; i < vector::size(si.animations); ++i)
					{
						const u32 num = skinny::expression_language::compile(si.animations[i].weight.c_str()
							 , num_variables
							 , variables
							 , num_constants
							 , constants
							 , constant_values
							 , array::begin(_byte_code) + written
							 , array::size(_byte_code)
							 );

						const_cast<AnimationInfo&>(si.animations[i]).bytecode_entry = num > 0 ? written : UINT32_MAX;
						written += num;
					}

					const u32 num = skinny::expression_language::compile(si.speed.c_str()
						 , num_variables
						 , variables
						 , num_constants
						 , constants
						 , constant_values
						 , array::begin(_byte_code) + written
						 , array::size(_byte_code)
						 );

					const_cast<StateInfo&>(si).speed_bytecode = num > 0 ? written : UINT32_MAX;
					written += num;
				}

				default_allocator().deallocate(variables);
			}
		}

		void write(CompileOptions& opts)
		{
			StateMachineResource smr;
			smr.version = RESOURCE_VERSION_STATE_MACHINE;
			smr.initial_state_offset = hash_map::get(_offsets, _initial_state, UINT32_MAX);
			smr.num_variables = vector::size(_variables);
			smr.variables_offset = _offset_accumulator._offset; // Offset of last state + 1
			smr.bytecode_size = array::size(_byte_code)*4;
			smr.bytecode_offset = smr.variables_offset + smr.num_variables*4*2;
			opts.write(smr.version);
			opts.write(smr.initial_state_offset);
			opts.write(smr.num_variables);
			opts.write(smr.variables_offset);
			opts.write(smr.bytecode_size);
			opts.write(smr.bytecode_offset);

			// Write states
			auto cur = map::begin(_states);
			auto end = map::end(_states);
			for (; cur != end; ++cur)
			{
				const StateInfo& si = cur->pair.second;
				const u32 num_animations  = vector::size(si.animations);
				const u32 num_transitions = vector::size(si.transitions);

				// Write speed
				opts.write(si.speed_bytecode);

				// Write loop
				opts.write(si.loop);

				// Write transitions
				TransitionArray ta;
				ta.num = num_transitions;
				opts.write(ta.num);
				for (u32 i = 0; i < num_transitions; ++i)
				{
					Transition t = si.transitions[i].transition;
					t.state_offset = hash_map::get(_offsets, si.transitions[i].state, UINT32_MAX);

					opts.write(t.event);
					opts.write(t.state_offset);
					opts.write(t.mode);
				}

				// Write animations
				AnimationArray aa;
				aa.num = num_animations;
				opts.write(aa.num);
				for (u32 i = 0; i < num_animations; ++i)
				{
					Animation a;
					a.name = si.animations[i].name;
					a.bytecode_entry = si.animations[i].bytecode_entry;
					a.pad = 0;

					opts.write(a.name);
					opts.write(a.bytecode_entry);
					opts.write(a.pad);
				}
			}

			// Write variables
			for (u32 i = 0; i < vector::size(_variables); ++i)
				opts.write(_variables[i].name);
			for (u32 i = 0; i < vector::size(_variables); ++i)
				opts.write(_variables[i].value);

			// Write bytecode
			for (u32 i = 0; i < array::size(_byte_code); ++i)
				opts.write(_byte_code[i]);
		}
	};

	void compile(const char* path, CompileOptions& opts)
	{
		Buffer buf = opts.read(path);

		StateMachineCompiler smc(opts);
		smc.parse(buf);
		smc.write(opts);
	}

} // namespace state_machine_internal

namespace state_machine
{
	const State* initial_state(const StateMachineResource* smr)
	{
		return (State*)((char*)smr + smr->initial_state_offset);
	}

	const State* state(const StateMachineResource* smr, const Transition* t)
	{
		return (State*)((char*)smr + t->state_offset);
	}

	const TransitionArray* state_transitions(const State* s)
	{
		return &s->ta;
	}

	const Transition* transition(const TransitionArray* ta, u32 index)
	{
		CE_ASSERT(index < ta->num, "Index out of bounds");
		const Transition* first = (Transition*)(&ta[1]);
		return &first[index];
	}

	const AnimationArray* state_animations(const State* s)
	{
		const TransitionArray* ta = state_transitions(s);
		const Transition* first = (Transition*)(&ta[1]);
		return (AnimationArray*)(first + ta->num);
	}

	const Animation* animation(const AnimationArray* aa, u32 index)
	{
		CE_ASSERT(index < aa->num, "Index out of bounds");
		Animation* first = (Animation*)(&aa[1]);
		return &first[index];
	}

	static inline const StringId32* variables_names(const StateMachineResource* smr)
	{
		return (StringId32*)((char*)smr + smr->variables_offset);
	}

	const f32* variables(const StateMachineResource* smr)
	{
		const StringId32* names = variables_names(smr);
		return (f32*)(names + smr->num_variables);
	}

	u32 variable_index(const StateMachineResource* smr, StringId32 name)
	{
		const StringId32* names = variables_names(smr);
		for (u32 i = 0; i < smr->num_variables; ++i)
		{
			if (names[i] == name)
				return i;
		}

		return UINT32_MAX;
	}

	const u32* byte_code(const StateMachineResource* smr)
	{
		return (u32*)((char*)smr + smr->bytecode_offset);
	}

} // namespace state_machine

} // namespace crown
