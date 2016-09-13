#include <list>
// Ponce
#include "callbacks.hpp"
#include "globals.hpp"
#include "context.hpp"
#include "utils.hpp"
#include "tainting_n_symbolic.hpp"
#include "blacklisted.hpp"
#include "actions.hpp"

//IDA
#include <ida.hpp>
#include <dbg.hpp>
#include <loader.hpp>
#include <intel.hpp>

//Triton
#include "api.hpp"
#include "x86Specifications.hpp"

std::list<breakpoint_pending_action> breakpoint_pending_actions;

/*This function will create and fill the Triton object for every instruction*/
void tritonize(ea_t pc, thid_t threadID)
{
	/*Check tha the runtime Trigger is on just in case*/
	if (!ponce_runtime_status.runtimeTrigger.getState())
		return;

	//We delete the last_instruction
	if (ponce_runtime_status.last_triton_instruction != NULL)
		delete ponce_runtime_status.last_triton_instruction;

	triton::arch::Instruction* tritonInst = new triton::arch::Instruction();
	ponce_runtime_status.last_triton_instruction = tritonInst;

	/*This will fill the 'cmd' (to get the instruction size) which is a insn_t structure https://www.hex-rays.com/products/ida/support/sdkdoc/classinsn__t.html */
	if (!decode_insn(pc))
		warning("[!] Some error decoding instruction at %p", pc);	
	
	unsigned char opcodes[15];
	get_many_bytes(pc, opcodes, sizeof(opcodes));

	/* Setup Triton information */
	tritonInst->partialReset();
	tritonInst->setOpcodes((triton::uint8*)opcodes, cmd.size);
	tritonInst->setAddress(pc);
	tritonInst->setThreadId(threadID);

	/* Disassemble the instruction */
	try{
		triton::api.disassembly(*tritonInst);
	}
	catch (...){
		msg("[!] Dissasembling error at " HEX_FORMAT " Opcodes:",pc);
		for (auto i = 0; i < cmd.size; i++)
			msg("%2x ", *(unsigned char*)(opcodes + i));
		msg("\n");
		return;
	}
	if (cmdOptions.showDebugInfo)
		msg("[+] Triton At " HEX_FORMAT ": %s (Thread id: %d)\n", pc, tritonInst->getDisassembly().c_str(), threadID);

	/* Process the IR and taint */
	triton::api.buildSemantics(*tritonInst);

	/*In the case that the snapshot engine is in use we shoudl track every memory write access*/
	if (snapshot.exists())
	{
		auto store_access_list = tritonInst->getStoreAccess();
		for (auto it = store_access_list.begin(); it != store_access_list.end(); it++)
		{
			triton::arch::MemoryAccess memory_access = it->first;
			auto addr = memory_access.getAddress();
			//This is the way to force IDA to read the value from the debugger
			//More info here: https://www.hex-rays.com/products/ida/support/sdkdoc/dbg_8hpp.html#ac67a564945a2c1721691aa2f657a908c
			invalidate_dbgmem_contents((ea_t)addr, memory_access.getSize()); //ToDo: Do I have to call this for every byte in memory I want to read?
			for (unsigned int i = 0; i < memory_access.getSize(); i++)
			{
				triton::uint128 value = 0;
				//We get the memory readed
				get_many_bytes((ea_t)addr+i, &value, 1);
				//We add a meomory modification to the snapshot engine
				snapshot.addModification((ea_t)addr + i, value.convert_to<char>());
			}
		}
	}

	if (cmdOptions.addCommentsControlledOperands)
		get_controlled_operands_and_add_comment(tritonInst, pc);

	if (cmdOptions.addCommentsSymbolicExpresions)
		add_symbolic_expressions(tritonInst, pc);

	/* Trust operands */
	for (auto op = tritonInst->operands.begin(); op != tritonInst->operands.end(); op++)
		op->setTrust(true);

	if (cmdOptions.paintExecutedInstructions)
		set_item_color(pc, cmdOptions.color_executed_instruction);

	//ToDo: The isSymbolized is missidentifying like "user-controlled" some instructions: https://github.com/JonathanSalwan/Triton/issues/383
	if (tritonInst->isTainted() || tritonInst->isSymbolized())
	{
		if (cmdOptions.showDebugInfo)
			msg("[!] Instruction %s at " HEX_FORMAT "\n", tritonInst->isTainted()? "tainted": "symbolized", pc);
		if (cmdOptions.RenameTaintedFunctionNames)
			rename_tainted_function(pc);
		if (tritonInst->isBranch()) // Check if it is a conditional jump
			set_item_color(pc, cmdOptions.color_tainted_condition);
		else
			set_item_color(pc, cmdOptions.color_tainted);
	}

	if (tritonInst->isBranch() && tritonInst->isSymbolized())
	{
		ea_t addr1 = (ea_t)tritonInst->getNextAddress();
		ea_t addr2 = (ea_t)tritonInst->operands[0].getImmediate().getValue();
		if (cmdOptions.showDebugInfo)
			msg("[+] Branch symbolized detected at " HEX_FORMAT ": " HEX_FORMAT " or " HEX_FORMAT ", Taken:%s\n", pc, addr1, addr2, tritonInst->isConditionTaken() ? "Yes" : "No");
		ea_t ripId = triton::api.getSymbolicRegisterId(TRITON_X86_REG_PC);
		if (tritonInst->isConditionTaken())
			ponce_runtime_status.myPathConstraints.push_back(PathConstraint(ripId, pc, addr2, addr1, ponce_runtime_status.myPathConstraints.size()));
		else
			ponce_runtime_status.myPathConstraints.push_back(PathConstraint(ripId, pc, addr1, addr2, ponce_runtime_status.myPathConstraints.size()));
	}
	//We add the instruction to the map, so we can use it later to negate conditions, view SE, slicing, etc..
	//instructions_executed_map[pc].push_back(tritonInst);
}

/*This function is called when we taint a register that is used in the current instruction*/
void reanalize_current_instruction()
{
	uint64 eip;
	get_reg_val("eip", &eip);
	if (cmdOptions.showDebugInfo)
		msg("[+] Reanalizyng instruction at " HEX_FORMAT "\n", eip);
	tritonize((ea_t)eip, get_current_thread());
}

/*This functions is called every time a new debugger session starts*/
void triton_restart_engines()
{
	if (cmdOptions.showDebugInfo)
		msg("[+] Restarting triton engines...\n");
	//We reset everything at the beginning
	triton::api.resetEngines();
	//If we are in taint analysis mode we enable only the tainting engine and disable the symbolic one
	triton::api.getTaintEngine()->enable(cmdOptions.use_tainting_engine);
	triton::api.getSymbolicEngine()->enable(cmdOptions.use_symbolic_engine);
	//triton::api.getSymbolicEngine()->enable(true);
	ponce_runtime_status.runtimeTrigger.disable();
	ponce_runtime_status.is_something_tainted_or_symbolize = false;
	ponce_runtime_status.tainted_functions_index = 0;
	//Reset instruction counter
	ponce_runtime_status.total_number_traced_ins = ponce_runtime_status.current_trace_counter = 0;
	breakpoint_pending_actions.clear();
	set_automatic_taint_n_simbolic();
	ponce_runtime_status.myPathConstraints.clear();
}

int idaapi tracer_callback(void *user_data, int notification_code, va_list va)
{
	if (cmdOptions.showExtraDebugInfo)
		msg("[+] Notification code: %d str: %s\n",notification_code, notification_code_to_string(notification_code).c_str());
	switch (notification_code)
	{
		case dbg_process_start:
		{
			if (cmdOptions.showDebugInfo)
				msg("[+] Starting the debugged process. Reseting all the engines.\n");
			triton_restart_engines();
			clear_requests_queue();
			break;
		}
		case dbg_step_into:
		case dbg_step_over:
		{
			//If the trigger is disbaled then the user is manually stepping with the ponce tracing disabled
			if (!ponce_runtime_status.runtimeTrigger.getState())
				break;
			//We want to enable the user to do step into/over, so he could choose whitch functions skip and with conditions negate
			debug_event_t* debug_event = va_arg(va, debug_event_t*);
			thid_t tid = debug_event->tid;
			ea_t pc = debug_event->ea;
			msg("Step over at"HEX_FORMAT"\n", pc);
			if (!decode_insn(pc))
				warning("[!] Some error decoding instruction at " HEX_FORMAT, pc);
			
			//We need to check if the instruction has been analyzed already. This happens when we are stepping into/over and 
			//we find a breakpoint we set (main, recv, fread), we are receiving two events: dbg_bpt and dbg_step_into for the 
			//same instruction. And we want to tritonize in dbg_bpt for example when we put bp in main and we execute the program
			if (ponce_runtime_status.last_triton_instruction != NULL && ponce_runtime_status.last_triton_instruction->getAddress() != pc)
			{
				if (cmdOptions.showExtraDebugInfo)
					msg("[+] Stepping %s: " HEX_FORMAT " (Tid: %d)\n", notification_code == dbg_step_into ? "into" : "over", pc, tid);
				tritonize(pc, tid);
			}
			break;
		}
		case dbg_trace:
		{
			//If the trigger is disbaled then the user is manually stepping with the ponce tracing disabled
			if (!ponce_runtime_status.runtimeTrigger.getState())
				break;

			thid_t tid = va_arg(va, thid_t);
			ea_t pc = va_arg(va, ea_t);
			msg("Dgb trace at" HEX_FORMAT "\n", pc);
			//Sometimes the cmd structure doesn't correspond with the traced instruction
			//With this we are filling cmd with the instruction at the address specified
			ua_ana0(pc);

			// We do this to blacklist API that does not change the tainted input
			if (cmd.itype == NN_call || cmd.itype == NN_callfi || cmd.itype == NN_callni)
			{
				qstring callee = get_callee(pc);
				unsigned int number_items = sizeof(black_func) / sizeof(char *);
				for (unsigned int i = 0; i < number_items; i++)
				{
					if (strcmp(callee.c_str(), black_func[i]) == 0)
					{
						//We are in a call to a blacklisted function.
						/*We should set a BP in the next instruction right after the
						blacklisted callback to enable tracing again*/
						ea_t next_ea = next_head(pc, BADADDR);
						add_bpt(next_ea, 1, BPT_EXEC);
						char cmt[256];
						sprintf_s(cmt, "Temporal bp set by ponce for blacklisting\n");
						//We set a comment so the user know why there is a new bp there
						set_cmt(next_ea, cmt, false);

						breakpoint_pending_action bpa;
						bpa.address = next_ea;
						bpa.ignore_breakpoint = false;
						bpa.callback = enableTrigger; // We will enable back the trigger when this bp get's reached
						
						//We add the action to the list
						breakpoint_pending_actions.push_back(bpa);

						//Disabling step tracing...
						disable_step_trace();
						
						//We want to tritonize the call, so the memory write for the ret address in the stack will be restore by the snapshot
						tritonize(pc, tid);
						ponce_runtime_status.runtimeTrigger.disable();

						return 0;
					}
				}
			}
			//If the instruciton is not a blacklisted call we analyze the instruction
			//We don't want to reanalize instructions. p.e. if we put a bp we receive two events, the bp and this one
			if (ponce_runtime_status.last_triton_instruction != NULL && ponce_runtime_status.last_triton_instruction->getAddress() != pc)
				tritonize(pc, tid);

			ponce_runtime_status.current_trace_counter++;
			ponce_runtime_status.total_number_traced_ins++;

			//This is the wow64 switching, we need to skip it. https://forum.hex-rays.com/viewtopic.php?f=8&t=4070
			if (ponce_runtime_status.last_triton_instruction->getDisassembly().find("call dword ptr fs:[0xc0]") != -1)
			{
				if (cmdOptions.showExtraDebugInfo)
					msg("[+ ] Wow64 switching! Requesting a step_over\n");
				//And now we need to stop the tracing, do step over and reenable the tracing...
				//disable_step_trace();
				suspend_process();
				request_step_over();
				request_continue_process();
				run_requests();
				break;
			}

			if (cmdOptions.limitInstructionsTracingMode && ponce_runtime_status.current_trace_counter >= cmdOptions.limitInstructionsTracingMode)
			{
				int answer = askyn_c(1, "[?] %d instructions has been traced. Do you want to execute %d more?", ponce_runtime_status.total_number_traced_ins, cmdOptions.limitInstructionsTracingMode);
				if (answer == 0 || answer == -1) //No or Cancel
				{
					// stop the trace mode and suspend the process
					enable_step_trace(false);
					suspend_process();
					msg("[!] Process suspended (Traced %d instructions)\n", ponce_runtime_status.total_number_traced_ins);
				}
				else
				{
					ponce_runtime_status.current_trace_counter = 0;
				}
			}
			break;
		}
		case dbg_bpt:
		{
			thid_t tid = va_arg(va, thid_t);
			ea_t pc = va_arg(va, ea_t);
			msg("Dgb bptat"HEX_FORMAT"\n", pc);
			int *warn = va_arg(va, int *);
			//This variable defines if a breakpoint is a user-defined breakpoint or not
			bool user_bp = true;
			msg("Breakpoint reached! At " HEX_FORMAT "\n", pc);
			//We look if there is a pending action for this breakpoint
			for (auto it = breakpoint_pending_actions.begin(); it != breakpoint_pending_actions.end(); ++it)
			{
				breakpoint_pending_action bpa = *it;
				//If we find a pendign action we execute the callback
				if (pc == bpa.address)
				{
					bpa.callback(pc);
					tritonize(pc, tid);
					//If there is a user-defined bp in the same address we should respect it and dont continue the exec
					if (!bpa.ignore_breakpoint)
					{
						//If it's a breakpoint the plugin set not a user-defined bp
						user_bp = false;
						//If not this is the bp we set to taint the arguments, we should rmeove it and continue the execution
						del_bpt(pc);
						enable_step_trace(true);
						//We dont want to skip library funcions or debug segments
						set_step_trace_options(0);
						continue_process();
						//We delete the comment
						set_cmt(pc, "", false);

						breakpoint_pending_actions.erase(it);
					}
					break;
				}
			}
			//If it is a user break point we enable again the step tracing if it was enabled previously...
			//The idea is if the user uses Execute native til next bp, and IDA reachs the next bp we reenable the tracing
			if (user_bp)
			{
				//request_suspend_process();
				//run_requests();
				//disable_step_trace();
				//request_enable_step_trace();
				//If the trigger is disabled then the user is manually stepping with the ponce tracing disabled
				//if (ponce_runtime_status.runtimeTrigger.getState())
				//enable_step_trace(ponce_runtime_status.runtimeTrigger.getState());
			}
			break;
		}
		case dbg_process_exit:
		{
			if (cmdOptions.showDebugInfo)
				msg("[!] Process_exiting...\n");
			//Do we want to unhook this event? I don't think so we want to be hooked for future sessions
			//unhook_from_notification_point(HT_DBG, tracer_callback, NULL);
			ponce_runtime_status.runtimeTrigger.disable();
			break;
		}
	}
	return 0;
}

//---------------------------------------------------------------------------
// Callback for ui notifications
int idaapi ui_callback(void * ud, int notification_code, va_list va)
{
	switch (notification_code)
	{
		// Called when IDA is preparing a context menu for a view
		// Here dynamic context-depending user menu items can be added.
		case ui_populating_tform_popup:
		{
			TForm *form = va_arg(va, TForm *);
			TPopupMenu *popup_handle = va_arg(va, TPopupMenu *);
			int view_type= get_tform_type(form);

			//Adding a separator
			attach_action_to_popup(form, popup_handle, "");

			/*Iterate over all the actions*/			
			for (int i = 0;; i++)
			{
				if (action_list[i].action_decs == NULL)
					break;
			
				/*Iterate over the view types of every action*/
				for (int j=0;; j++)
				{
					if (action_list[i].view_type[j] == NULL){
						break;
					}
					if (action_list[i].view_type[j] == view_type)
					{
						//We only attach to the popup if the action makes sense with the current configuration
						if (cmdOptions.use_tainting_engine && action_list[i].enable_taint || cmdOptions.use_symbolic_engine && action_list[i].enable_symbolic)
						{
							attach_action_to_popup(form, popup_handle, action_list[i].action_decs->name, action_list[i].menu_path, SETMENU_APP);
						}
					}
				}	
			}

			//Adding a separator
			attach_action_to_popup(form, popup_handle, "");
			break;
		}
		case ui_finish_populating_tform_popup:
		{
			//This event is call after all the Ponce menus have been added and updated
			//It is the perfect point to add the multiple condition solve submenus
			TForm *form = va_arg(va, TForm *);
			TPopupMenu *popup_handle = va_arg(va, TPopupMenu *);
			int view_type = get_tform_type(form);
			//We get the ea form a global variable that is set in the update event
			//This is not very elegant but I don't know how to do it from here
			ea_t cur_ea = popup_menu_ea;
			if (view_type == BWN_DISASM)
			{
				//Adding submenus for solve with all the conditions executed in the same address
				for (unsigned int i = 0; i < ponce_runtime_status.myPathConstraints.size(); i++)
				{
					//We should filter here for the ea
					if (cur_ea == ponce_runtime_status.myPathConstraints[i].conditionAddr)
					{
						char name[256];
						//We put the index at the beginning so it is very easy to parse it with atoi(action_name)
						sprintf_s(name, "%d_Ponce:solve_formula_sub", i);
						action_IDA_solve_formula_sub.name = name;
						char label[256];
						sprintf_s(label, "%d. " HEX_FORMAT " -> " HEX_FORMAT "", ponce_runtime_status.myPathConstraints[i].bound, ponce_runtime_status.myPathConstraints[i].conditionAddr, ponce_runtime_status.myPathConstraints[i].takenAddr);
						action_IDA_solve_formula_sub.label = label;
						bool success = register_action(action_IDA_solve_formula_sub);
						//If the submenu is already registered, we should unregister it and re-register it
						if (!success)
						{
							unregister_action(action_IDA_solve_formula_sub.name);
							success = register_action(action_IDA_solve_formula_sub);
						}
						success = attach_action_to_popup(form, popup_handle, action_IDA_solve_formula_sub.name, "SMT/Solve formula/", SETMENU_APP);
					}
				}
			}
			break;
		}
		case dbg_process_exit:
		{
			unhook_from_notification_point(HT_DBG, ui_callback, NULL);
			break;
		}
	}
	return 0;
}

/*We set the memory to the results we got and do the analysis from there*/
void set_SMT_results(Input *input_ptr){
	/*To set the memory types*/
	for (auto it = input_ptr->memOperand.begin(); it != input_ptr->memOperand.end(); it++)
		put_many_bytes((ea_t)it->getAddress(), &it->getConcreteValue(), it->getSize());	
		
	/*To set the register types*/
	for (auto it = input_ptr->regOperand.begin(); it != input_ptr->regOperand.end(); it++)
		set_reg_val(it->getName().c_str(), it->getConcreteValue().convert_to<uint64>());
		
	if (cmdOptions.showDebugInfo)
		msg("[+] Memory/Registers set with the SMT results\n");
}
