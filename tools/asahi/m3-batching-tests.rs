// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Host tests of the production batch admission and lifetime policies.
//! Run: rustc --edition=2021 --test tools/asahi/m3-batching-tests.rs -o /tmp/m3-batching-tests
#![allow(dead_code)]
#[path = "../../drivers/gpu/drm/asahi/m3_batch_policy.rs"]
mod policy;
#[path = "../../drivers/gpu/drm/asahi/m3_timeline.rs"]
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
    assert!(!may_join(true,0,true,false));
    assert!(!may_join(true,1,true,true));
    assert!(!may_join(true,0,false,true));
    assert!(!may_join(false,0,true,true));
    assert!(may_join(true,0,true,true));
    // A sorted pending head cannot detect its own urgency advantage. Compare
    // with the active batch so same-VM urgent work also stops extending it.
    assert!(urgent_waiting([3,0].into_iter(),0));
    assert!(!urgent_waiting([3,0].into_iter(),3));
    assert!(!urgent_waiting([0,0].into_iter(),0));
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
