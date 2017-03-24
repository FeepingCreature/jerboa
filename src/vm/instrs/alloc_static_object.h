#ifndef FN_NAME
#define FN_NAME vm_instr_alloc_static_object
#define FN_NAME_DEFINED
#endif

#include "core.h"
#include "object.h"
#include "vm/vm.h"

static FnWrap FN_NAME(VMState *state) FAST_FN;
static FnWrap FN_NAME(VMState * __restrict__ state) {
  AllocStaticObjectInstr * __restrict__ asoi = (AllocStaticObjectInstr*) state->instr;
  Callframe * __restrict__ frame = state->frame;
  
  Slot target_slot = asoi->target_slot, parent_slot = asoi->parent_slot;
  Object *parent_obj = closest_obj(state, read_slot(frame, parent_slot));
  
#if defined(ENTRIES_NUM) && defined(ENTRIES_STORED) && defined(STACK)
  static const int entries_stored = ENTRIES_STORED;
  static const int tbl_num = ENTRIES_NUM;
  static const bool alloc_stack = STACK;
  VM_ASSERT2_DEBUG(asoi->tbl.entries_stored == entries_stored, "1. instr specialization error: %i != %i", asoi->tbl.entries_stored, entries_stored);
  VM_ASSERT2_DEBUG(tbl_num == asoi->tbl.entries_num, "2. instr specialization error: %i != %i", tbl_num, asoi->tbl.entries_num);
  VM_ASSERT2_DEBUG(alloc_stack == asoi->alloc_stack, "3. instr specialization error: %i != %i", alloc_stack, asoi->alloc_stack);
#else
  int entries_stored = asoi->tbl.entries_stored;
  int tbl_num = asoi->tbl.entries_num;
  bool alloc_stack = asoi->alloc_stack;
#endif
  
  Object * __restrict__ obj = (Object*) alloc_object_internal(state, sizeof(Object) + sizeof(TableEntry) * tbl_num, alloc_stack);
  if (UNLIKELY(!obj)) return (FnWrap) { vm_halt }; // oom, possibly stack oom
  
  TableEntry * __restrict__ obj_entries_ptr = (TableEntry*) ((Object*) obj + 1); // fixed table, hangs off the end
  __builtin_prefetch(obj_entries_ptr, 1 /* write */, 1 /* 1/3 locality */);
  
  VM_ASSERT2(!parent_obj || !(parent_obj->flags & OBJ_NOINHERIT), "cannot inherit from object marked no-inherit");
  obj->parent = parent_obj;
  
  // TODO don't gen instr if 0
  obj->tbl = asoi->tbl;
  obj->tbl.entries_ptr = obj_entries_ptr;
  obj->flags = (ObjectFlags) (OBJ_CLOSED | OBJ_INLINE_TABLE);
  bzero(obj_entries_ptr, sizeof(TableEntry) * tbl_num);
  
  StaticFieldInfo * __restrict__ info = ASOI_INFO(asoi);
  for (int i = 0; i != entries_stored; i++, info++) {
    TableEntry * __restrict__ entry = (TableEntry*) ((char*) obj_entries_ptr + info->offset);
    __builtin_prefetch(entry, 1 /* write */, 1 /* 1/3 locality */);
    // fprintf(stderr, ":: %p\n", (void*) &entry->value);
    uint32_t hash = info->key.hash;
    Object *constraint = info->constraint;
    TableEntry **refslot = get_refslot_ref(frame, info->refslot);
    Value value = read_slot(frame, info->slot);
    VM_ASSERT2(!constraint || value_should_be_instance_of(state, value, constraint), "type constraint violated on variable");
    *refslot = entry;
    entry->hash = hash;
    entry->constraint = constraint;
    entry->value = value;
  }
  
  write_slot(frame, target_slot, OBJ2VAL(obj));
  
  // fprintf(stderr, "%i = %li + %li * %i\n", instr_size(state->instr), sizeof(AllocStaticObjectInstr), sizeof(StaticFieldInfo), entries_stored);
  state->instr = (Instr*)((char*) asoi
                          + sizeof(AllocStaticObjectInstr)
                          + sizeof(StaticFieldInfo) * entries_stored);
  STEP_VM;
}

#ifdef FN_NAME_DEFINED
#undef FN_NAME_DEFINED
#undef FN_NAME
#endif
