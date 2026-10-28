#include "bbscript.h"
#include "arcsys.h"
#include <cctype>
#include <cstring>

#include "sigscan.h"

namespace bbscript {
    short* instruction_sizes;
    bool available = false;

    // get_operand_value(obj, value) returns the value's current number.
    using get_operand_value_t = int(*)(char* obj, char* operand);
    get_operand_value_t get_operand_value;

	// Found in the running game's code: the game's own script walker reads each
	// instruction's size from this table, and get_operand_value resolves the
	// variables some instructions use. If either can't be found or the table doesn't
	// look right, `available` stays false and nothing reads scripts.
	void BBSInitializeFunctions()
	{
	    available = false;
	    try
	    {
	        const uintptr_t walker = sigscan::get().scan(
	            "\xBE\x02\x20\x00\x3B\x48\x8D\x2D\x00\x00\x00\x00\x41\x8B\x00\x83\xF8\x02\x74\x55",
	            "xxxxxxxx????xxxxxxxx");
	        instruction_sizes = static_cast<short*>(get_rip_relative(walker + 8));
	        get_operand_value = reinterpret_cast<get_operand_value_t>(sigscan::get().scan(
	            "\x41\x56\x48\x83\xEC\x30\x83\x3A\x00\x4C\x8B\xF1\x75\x0A\x8B\x42\x04\x48\x83\xC4\x30",
	            "xxxxxxxxxxxxxxxxxxxxx"));
	        available = instruction_sizes && get_operand_value &&
	                    instruction_sizes[0] == 0x24 && instruction_sizes[1] == 4 && instruction_sizes[2] == 0x28;
	    }
	    catch (...) { available = false; }
	}

	bool scriptReaderAvailable() { return available; }

	namespace {
	    constexpr int kMaxSteps = 2048;  // no state script is anywhere near this long
	    // Size of the instruction at p, or 0 if it doesn't look like one.
	    int instructionSize(const char* p)
	    {
	        const unsigned int op = *reinterpret_cast<const unsigned int*>(p);
	        if (op >= static_cast<unsigned int>(opcode::MAX)) return 0;
	        const int size = instruction_sizes[op];
	        return (size > 0 && size % 4 == 0) ? size : 0;
	    }
	}

	void code_pointer::read_script()
	{
	    state_remaining_time = 0;
	    ok = false;
	    if (!available || !ptr || !owner) return;
	    for (int step = 0; step < kMaxSteps; step++)
	    {
	        const int op = *reinterpret_cast<int*>(ptr);
	        if (op == static_cast<int>(opcode::end_state) || op == static_cast<int>(opcode::exit_state)) { ok = true; return; }
	        bool is_begin = false;
	        for (const auto BeginEndPair : BeginEndPairs)
	        {
	            if (op == BeginEndPair[0]) { is_begin = true; break; }
	        }
	        if (is_begin)
	        {
	            if (!get_skip_begin_end_addr()) return;
	            continue;
	        }
	        const int size = instructionSize(ptr);
	        if (!size) return;
	        execute_instruction(op);
	        ptr += size;
	        if (nandemo) { ok = true; return; }
	    }
	}

	void code_pointer::execute_instruction(int code)
	{
		switch (code)
		{
		case static_cast<int>(opcode::set_sprite):
			{
				const int sprite_time = last_sprite_time = *reinterpret_cast<int*>(ptr + 36);
				state_remaining_time += sprite_time;
				break;
			}
		case static_cast<int>(opcode::call_subroutine):
			{
				char subroutine_name[33] = {};
				memcpy(subroutine_name, ptr + 4, 32);
				if (strstr(subroutine_name, "Nandemo") != nullptr)
				{
					nandemo = true;
					state_remaining_time -= last_sprite_time;
				}
				break;
			}
		case static_cast<int>(opcode::set_sprite_time):
			{
				char* operand = ptr + 8;
				if (get_operand_value(owner, operand))
				{
					state_remaining_time -= last_sprite_time;
					last_sprite_time = *reinterpret_cast<int*>(ptr + 4);
					state_remaining_time += last_sprite_time;
				}
				break;
			}
		case static_cast<int>(opcode::super_freeze):
			{
				const int self_freeze = last_sprite_time = *reinterpret_cast<int*>(ptr + 12);
				last_sprite_time -= self_freeze;
				state_remaining_time -= self_freeze;
				break;
			}
		case static_cast<int>(opcode::sprite_time_add):
			{
				last_sprite_time += *reinterpret_cast<int*>(ptr + 4);
				state_remaining_time += *reinterpret_cast<int*>(ptr + 4);
				break;
			}
            default: break;
        }
	}

    bool code_pointer::get_skip_begin_end_addr()
	{
	    unsigned int indent = 0;
	    int step = 0;
	    do
	    {
	        if (++step > kMaxSteps) return false;
	        const int op = *reinterpret_cast<int*>(ptr);
	        bool begin = true;
	        for (const auto BeginEndPair : BeginEndPairs)
	        {
	            if (op == BeginEndPair[0])
	            {
	                begin = false;
	            }
	        }
	        unsigned int temp_indent = indent + 1;
	        if (!begin)
	            temp_indent = indent;
	        bool end = true;
	        for (const auto BeginEndPair : BeginEndPairs)
	        {
	            if (op == BeginEndPair[1])
	            {
	                end = false;
	            }
	        }
	        const int opcode_size = instructionSize(ptr);
	        if (!opcode_size) return false;
	        indent = temp_indent - 1;
	        if (!end)
	            indent = temp_indent;
	        ptr += opcode_size;
	    }
	    while (indent);
	    return true;
	}
}  // bbscript
