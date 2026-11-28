// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Host tests of the production batch admission and lifetime policies.
//! Run: rustc --edition=2021 --test tools/asahi/m3-batching-tests.rs -o /tmp/m3-batching-tests
#![allow(dead_code)]
#[path = "../../drivers/gpu/drm/asahi/m3/m3_batch_policy.rs"]
mod policy;
#[path = "../../drivers/gpu/drm/asahi/m3/m3_timeline.rs"]
mod timeline;
use policy::*;

#[test]
fn priority_fifo_and_partial_packet_yields() {
    let mut queue=vec![(10,0),(11,0),(12,1)];
    // Match the scheduler's descending urgency, retaining FIFO for equals.
    queue.sort_by_key(|(_,p)|std::cmp::Reverse(*p));
    for (id,urgency) in [(13,1),(14,3),(15,0)] {
        let at=pending_position(queue.iter().map(|(_,p)|*p),urgency);
        queue.insert(at,(id,urgency));
    }
    assert_eq!(queue,[(14,3),(12,1),(13,1),(10,0),(11,0),(15,0)]);
    // An unfinished low-priority packet gets a high-priority predecessor next batch.
    let low_continuation=vec![0,0];
    assert_eq!(pending_position(low_continuation.into_iter(),3),0);
    assert!(!may_join(0,true,false));
    assert!(!may_join(1,true,true));
    assert!(!may_join(0,false,true));
    assert!(may_join(0,true,true));
    // A sorted pending head cannot detect its own urgency advantage. Compare
    // with the active batch so same-VM urgent work also stops extending it.
    assert!(urgent_waiting([3,0].into_iter(),0));
    assert!(!urgent_waiting([3,0].into_iter(),3));
    assert!(!urgent_waiting([0,0].into_iter(),0));
}

#[test]
fn compute_batches_grow_only_under_a_budget() {
    let budget=Some(4_000_000);
    for joined in [false,true] { for explicit in [false,true] {
        // One command always goes.
        assert!(admits(false,joined,explicit,None,None,0));
        // A measured cost that fits the budget lets the batch grow.
        assert!(admits(false,joined,explicit,Some(1_000_000),budget,3));
        assert!(admits(false,joined,explicit,Some(10_000),budget,1));
    }}
    // Without an override: no budget or no measurement keeps one command per batch.
    for joined in [false,true] {
        assert!(!admits(false,joined,false,Some(10_000),None,1));
        assert!(!admits(false,joined,false,None,budget,1));
        assert!(!admits(false,joined,false,Some(1_000_000),budget,4));
    }
    // An explicit override bounds the front packet's own commands alone, never joined ones.
    assert!(admits(false,false,true,None,None,15));
    assert!(admits(false,false,true,Some(10_000_000),budget,15));
    assert!(!admits(false,true,true,None,None,1));
    assert!(!admits(false,true,true,Some(1_000_000),budget,4));
    // Render keeps its budget rule.
    for joined in [false,true] {
        assert!(admits(true,joined,false,None,budget,0));
        assert!(!admits(true,joined,false,None,budget,1));
        assert!(admits(true,joined,false,None,None,7));
    }
}

#[test]
fn batched_commands_keep_their_deadline() {
    assert_eq!(batch_deadline_secs(0),2);
    assert_eq!(batch_deadline_secs(1),2);
    assert_eq!(batch_deadline_secs(8),16);
    assert_eq!(batch_deadline_secs(16),32);
}

/// Commands a compute batch admits for a cost and budget, with no override (`next_batch()` asks
/// `admits` once per command; m3-batch-select-tests.py runs that selection itself).
fn compute_commands(joined:bool,cost:Option<u64>,budget:Option<u64>) -> usize {
    (0..SLOTS).take_while(|n|admits(false,joined,false,cost,budget,*n)).count()
}

#[test]
fn the_measured_compute_cost_sizes_the_batch() {
    let budget=Some(default_budget_us(true)*1000);
    for joined in [false,true] {
        let mut costs=PassCosts::default();
        // No measurement for the VM yet: one command.
        assert_eq!(compute_commands(joined,costs.get(1),budget),1);
        // A light dispatch (182 ns): only the slots bound the batch.
        costs.record(1,182);
        assert_eq!(compute_commands(joined,costs.get(1),budget),SLOTS);
        // Another VM's measurement doesn't count.
        assert_eq!(compute_commands(joined,costs.get(2),budget),1);
        // The measured cost caps the count: cost * commands stays within the 4 ms budget.
        for (ns,commands) in [(250_000,16),(500_000,8),(1_000_000,4),(1_500_000,2),(2_000_000,2),
            (2_000_001,1),(4_000_000,1),(200_000_000,1)] {
            let mut costs=PassCosts::default();
            costs.record(1,ns);
            assert_eq!(compute_commands(joined,costs.get(1),budget),commands,"{ns} ns");
        }
        // Without a budget every batch is one command, measured or not.
        assert_eq!(compute_commands(joined,Some(182),None),1);
        assert_eq!(compute_commands(joined,None,None),1);
    }
}

#[test]
fn a_stale_compute_cost_cannot_time_out_a_batch_of_valid_commands() {
    let budget=Some(default_budget_us(true)*1000);
    // A VM measured at 100 us per command then queues 200 ms dispatches: the stale cost still
    // admits a full batch.
    let mut costs=PassCosts::default();
    costs.record(1,100_000);
    let commands=compute_commands(true,costs.get(1),budget);
    assert_eq!(commands,SLOTS);
    // Its deadline is the sum of the deadlines the commands had alone, so the batch (3.2 s)
    // is in time, and so is any batch whose commands each fit their own deadline.
    let alone=batch_deadline_secs(1);
    for n in 1..=SLOTS {assert_eq!(batch_deadline_secs(n),alone*n as i64);}
    assert!(commands as i64*200<batch_deadline_secs(commands)*1000);
    // The batch's retirement corrects the estimate: the next batch is one command again.
    costs.record(1,200_000_000);
    assert_eq!(compute_commands(true,costs.get(1),budget),1);
}

#[test]
fn a_batch_that_fails_before_publication_leaves_new_packets_unpublished() {
    // (first command of the entry, published before the batch)
    struct Packet {published:bool,guard:bool}
    // The front packet continues at command 3 (an earlier batch published 0..3); two packets
    // join from their first command.
    let entries=[(0usize,3usize),(1,0),(2,0)];
    for overlap in [false,true] {
        let mut packets=[Packet{published:true,guard:true},Packet{published:false,guard:true},
            Packet{published:false,guard:true}];
        // Behind a running batch the packets are marked before encoding; otherwise only after.
        if overlap {for (p,_) in entries {packets[p].published=true;}}
        // Encoding fails: nothing reached the firmware.
        for (p,command) in entries {if first_publication(command) {packets[p].published=false;}}
        // A later failure of everything pending releases only the guards of unseen packets.
        for p in packets.iter_mut() {if release_unpublished(p.published) {p.guard=false;}}
        assert!(packets[0].guard,"the partly published front packet keeps its guard");
        assert!(!packets[1].guard && !packets[2].guard,"joined packets release theirs");
    }
    assert!(first_publication(0));
    assert!(!first_publication(1));
}

#[test]
fn initbm_vm_engine_and_notification_gates() {
    for bits in 0..32 {
        let yes=|i|bits&(1<<i)!=0;
        assert_eq!(overlap_ready(yes(0),yes(1),yes(2),yes(3),yes(4)),bits==31);
    }
}

#[test]
fn ring_tail_never_reuses_any_active_pass_exhaustive() {
    // Every possible contiguous live interval, including wrapping and full occupancy.
    for first in 0..SLOTS {
        for left in 1..=SLOTS {
            for right in 0..=SLOTS-left {
                let total=left+right;
                let (last_base,last_count,batches)=if right==0 {(first,left,1)}
                    else {((first+left)%SLOTS,right,2)};
                let room=pipeline_room(3,batches,total,last_base,last_count);
                if total==SLOTS {assert_eq!(room,None);continue;}
                let (base,max)=room.unwrap();
                assert_eq!(max,SLOTS-total);
                let live:Vec<_>=(0..total).map(|i|(first+i)%SLOTS).collect();
                for take in 1..=max {
                    let candidate:Vec<_>=(0..take).map(|i|(base+i)%SLOTS).collect();
                    assert!(candidate.iter().all(|slot|!live.contains(slot)));
                    assert!(total+take<=SLOTS);
                }
            }
        }
    }
    assert_eq!(pipeline_room(1,1,1,0,1),None);
    assert_eq!(pipeline_room(2,2,2,0,1),None);
    assert_eq!(pipeline_room(2,1,1,SLOTS,1),None);
    assert_eq!(pipeline_room(2,1,1,0,2),None);
}

#[test]
fn heavy_client_cannot_extend_compositor_wait_beyond_budget() {
    assert!(within_budget(Some(17_000_000),Some(4_000_000),0));
    assert!(!within_budget(Some(17_000_000),Some(4_000_000),1));
    // One ms/pass permits four passes total, counting already active batches.
    for passes in 0..SLOTS {
        assert_eq!(within_budget(Some(1_000_000),Some(4_000_000),passes),passes<4);
    }
    assert!(within_budget(None,Some(4_000_000),0));
    assert!(!within_budget(None,Some(4_000_000),1));
    assert!(within_budget(None,None,15));
    assert!(!within_budget(Some(u64::MAX),Some(4_000_000),15));
}

#[test]
fn recent_costs_keep_vm_identity_extremes_and_ignore_invalid_measurements() {
    let mut costs=PassCosts::default();
    costs.record(u64::MAX,0);
    assert_eq!(costs.get(u64::MAX),None);
    costs.record(u64::MAX,u64::MAX);
    costs.record(u64::MAX,u64::MAX);
    assert_eq!(costs.get(u64::MAX),Some(u64::MAX));
    costs.record(0,4_000_000);
    costs.record(0,8_000_000);
    assert_eq!(costs.get(0),Some(5_000_000));
    for id in 1..=6 {costs.record(id,1_000_000);}
    costs.record(u64::MAX,4_000_000); // refresh, then replace least recently measured
    costs.record(7,1_000_000);
    assert_eq!(costs.get(0),None);
    assert!(costs.get(u64::MAX).is_some());
    costs.record(7,0);
    assert_eq!(costs.get(7),Some(1_000_000));
}

#[test]
fn wrap_safe_completion_accepts_later_stamps_and_rejects_old_stamps() {
    for seed in [0x3d0000u32,0x7a0000] {
        for ordinal in [1u64,0x003e_ffff,0x00ff_fffe,0x00ff_ffff,0x0100_0000,
            u32::MAX as u64,1u64<<32] {
            let expected=timeline::stamp(seed,ordinal);
            assert!(timeline::reached(expected,expected));
            assert!(!timeline::reached(timeline::stamp(seed,ordinal-1),expected));
            for later in 1..=SLOTS as u64 {
                assert!(timeline::reached(timeline::stamp(seed,ordinal+later),expected));
            }
        }
    }
    assert_eq!(timeline::events(1u64<<31,2),0);
}

#[test]
fn cancelled_fence_does_not_release_live_guard_but_retirement_does() {
    use std::{cell::Cell,rc::Rc};
    struct Guard(Rc<Cell<usize>>);
    impl Drop for Guard {fn drop(&mut self){self.0.set(self.0.get()+1);}}
    for remaining in 0..=2 {for active_batches in 0..=2 {
        let dropped=Rc::new(Cell::new(0));
        let mut guard=Some(Guard(dropped.clone()));
        let mut pending=remaining>0;
        let mut active=active_batches;
        assert_eq!(packet_retired(pending,active>0),remaining==0 && active_batches==0);
        // Cancellation withdraws even an unpublished continuation; signaling leaves DMA.
        pending=false;
        if packet_retired(pending,active>0) {drop(guard.take());}
        assert_eq!(dropped.get(),usize::from(active==0));
        while active>0 {
            active-=1;
            if packet_retired(pending,active>0) {drop(guard.take());}
            assert_eq!(dropped.get(),usize::from(active==0));
        }
        // Retirement after an error signal releases exactly once, never re-signals it.
        drop(guard.take());assert_eq!(dropped.get(),1);
    }}
    assert!(!packet_retired(true,false)); // normal continuation retains the VM guard
}

#[test]
fn fault_teardown_never_frees_uncertain_dma() {
    for stopped in [true,false] {for published in [true,false] {
        assert_eq!(retain_runtime(!stopped,published),!stopped||published);
    }}
    // A partial first or second doorbell is published work, even with no Batch returned.
    for doorbells in 1..=2 {assert!(retain_runtime(false,doorbells>0));}
}

#[test]
fn experimental_air_is_active_while_pro_retains_serial_default() {
    assert_eq!(default_depth(true),2);
    assert_eq!(default_budget_us(true),4000);
    assert_eq!(default_depth(false),1);
    assert_eq!(default_budget_us(false),0);
    assert!(default_depth(true)<=SLOTS);
    assert_eq!(MAX_PACKETS,8);
}

#[test]
fn firmware_starts_once_per_boot() {
    let latch=StartLatch::new();
    assert!(latch.claim());
    // Unbind and rebind, a reload or a retry: every later start is refused.
    for _ in 0..3 {assert!(!latch.claim());}
    // Concurrent probes: exactly one claims it.
    let latch=std::sync::Arc::new(StartLatch::new());
    let claims:usize=(0..8).map(|_|{let l=latch.clone();std::thread::spawn(move||l.claim())})
        .collect::<Vec<_>>().into_iter().map(|t|usize::from(t.join().unwrap())).sum();
    assert_eq!(claims,1);
}

#[test]
fn failed_pending_packets_quarantine_only_what_the_firmware_may_have_seen() {
    // (in a batch fail_active drained, partly published and still pending, never in a batch)
    struct Packet {published:bool,guard:bool}
    let mut active=vec![Packet{published:true,guard:true}];
    let mut pending=vec![Packet{published:true,guard:true},Packet{published:false,guard:true}];
    // fail_active drains active before fail_pending: a check against active can't see these.
    let drained:Vec<_>=active.drain(..).collect();
    for p in pending.iter_mut() {if release_unpublished(p.published) {p.guard=false;}}
    assert!(drained[0].guard);
    assert!(pending[0].guard,"a partly published packet keeps its guard");
    assert!(!pending[1].guard,"a never-published packet releases its guard");
}

#[test]
fn the_start_pins_the_module_before_claiming_and_fails_closed() {
    use std::cell::Cell;
    let latch=StartLatch::new();
    let refs=Cell::new(1i32); // the module's base reference
    let pin_ok=||{refs.set(refs.get()+1);true};
    let unpin=||refs.set(refs.get()-1);
    // rmmod has begun: no pin, no claim, no start, and the reference count is untouched.
    assert_eq!(claim_start(&latch,||false,unpin),Err(StartRefusal::Removing));
    assert_eq!(refs.get(),1);
    // The first live start wins and keeps its pin.
    assert_eq!(claim_start(&latch,pin_ok,unpin),Ok(()));
    assert_eq!(refs.get(),2);
    // A later start (rebind, retry) loses the claim and drops only its own pin.
    assert_eq!(claim_start(&latch,pin_ok,unpin),Err(StartRefusal::AlreadyStarted));
    assert_eq!(refs.get(),2);
    // A later start racing rmmod still claims nothing.
    assert_eq!(claim_start(&latch,||false,||panic!("nothing to unpin")),Err(StartRefusal::Removing));
    assert_eq!(refs.get(),2);
}
