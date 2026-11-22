# SPDX-License-Identifier: MIT
"""Execute the actual wake_sensor body with synthetic status/registration mocks.

Every profile reads the status again after registration; with a nonce
session (J700) state 9 also asks for the patch, without one the patch
acknowledgement alone decides (registration::patch_loaded).
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

APPLE = Path(__file__).resolve().parents[3] / 'drivers/soc/apple'


class SensorWakeTest(unittest.TestCase):
    def test_post_registration_status_drives_patch_readiness(self):
        source = (APPLE / 'sbio.rs').read_text()
        start = source.index('    fn wake_sensor(')
        opening = source.index('{', start)
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        function = source[start:end]
        registration = (APPLE / 'registration.rs').read_text()
        start = registration.index('pub(crate) fn patch_loaded(')
        opening = registration.index('{', start)
        depth, end = 1, opening + 1
        while depth:
            depth += (registration[end] == '{') - (registration[end] == '}')
            end += 1
        patch_loaded = registration[start:end]
        program = r'''
#![allow(dead_code, unused_variables)]
use std::cell::RefCell;
macro_rules! dev_warn { ($($args:tt)*) => {} }
macro_rules! dev_info_refusal { ($($args:tt)*) => {} }
macro_rules! dev_warn_ratelimited { ($($args:tt)*) => {} }
macro_rules! dev_info_ratelimited { ($($args:tt)*) => {} }
macro_rules! dev_dbg { ($($args:tt)*) => {} }
macro_rules! dev_info { ($($args:tt)*) => {} }
use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
mod shim { pub fn boottime_ns()->u64 {0} }
mod module_parameters {
 pub struct Param(u8);
 impl Param { pub fn value(&self)->u8 {self.0} }
 #[allow(non_upper_case_globals)] pub static sensor_trace: Param = Param(0);
}
struct Profile { nonce_session: bool }
mod registration {
''' + patch_loaded + r'''
}
macro_rules! dev_err { ($($args:tt)*) => {} }
macro_rules! dev_err_ratelimited { ($($args:tt)*) => {} }
mod kernel { pub mod time {
 pub struct Delta;
 impl Delta { pub fn from_millis(_:i64)->Self {Self} }
 pub mod delay { pub fn fsleep(_:super::Delta) {} }
}}
#[derive(Clone, Copy)] struct PatchLoaded(());
#[derive(Default)] struct Mock {
 reads: usize, registered: bool, patch: Option<u8>, next: Option<sensor::Status>,
 /// The system begins to sleep once the serial is registered.
 suspend: bool,
}
thread_local! { static MOCK: RefCell<Mock> = RefCell::new(Mock::default()); }
mod sensor {
 pub const POWER_ON_READ_DELAYS_MS: [u32;1] = [0];
 pub const KNOWN_IDENTIFIERS: &[u16] = &[1];
 pub const PATCH_ACCEPTED: u8 = 0x29;
 pub const STATE_NEEDS_PATCH: u8 = 9;
 #[derive(Clone,Copy)] pub struct Status { pub state:u8, pub ack:u8, pub silent:bool }
 pub struct Identifier;
 pub enum Offset12 { Identifier(u16), Other }
 pub struct Timing;
 impl Timing { pub fn is_hardware(&self)->bool {true} pub fn name(&self)->&'static str {"mock"} }
 pub fn power_source() {}
 pub fn power_cycle()->bool {true}
 pub fn cs_timing()->Timing {Timing}
 pub fn status()->Result<Status,()> {
  super::MOCK.with(|m| { let mut m=m.borrow_mut(); m.reads+=1;
   if m.reads==1 {return Ok(Status{state:0,ack:PATCH_ACCEPTED,silent:false})}
   assert!(m.registered, "post-registration read preceded registration");
   m.next.ok_or(())
  })
 }
 impl Status {
  pub fn is_silent(&self)->bool {self.silent}
  pub fn offset12(&self)->Offset12 {Offset12::Identifier(1)}
  pub fn identifier_to_register(&self)->Option<Identifier> {Some(Identifier)}
  pub fn patch_ack(&self)->u8 {self.ack}
 }
}
struct Driver { profile: Profile, sensor_power_ns: AtomicU64 }
impl Driver {
 fn new()->Self { Driver { profile: Profile { nonce_session: false }, sensor_power_ns: AtomicU64::new(0) } }
 fn sensor_ms(&self)->u64 {0}
 fn register_sensor(&self,_:&sensor::Identifier)->bool {
  MOCK.with(|m| {let mut m=m.borrow_mut();assert_eq!(m.reads,1);m.registered=true;}); true
 }
 fn bringup_may_continue(&self,_:&std::ffi::CStr)->bool {
  MOCK.with(|m| !m.borrow().suspend)
 }
 fn apply_sensor_patch(&self,ack:u8)->Option<PatchLoaded> {
  MOCK.with(|m| {let mut m=m.borrow_mut();m.patch=Some(ack);});
  Some(PatchLoaded(()))
 }
'''
        program += function + '\n}\n' + r'''
fn main() {
 for (next,ready,patch) in [
  (Some(sensor::Status{state:9,ack:0x29,silent:false}),true,Some(0x29)),
  (None,false,None),
  (Some(sensor::Status{state:0,ack:0x29,silent:true}),false,None),
  (Some(sensor::Status{state:0,ack:0x29,silent:false}),true,None),
  (Some(sensor::Status{state:0,ack:0,silent:false}),true,Some(0)),
 ] {
  MOCK.with(|m| *m.borrow_mut()=Mock{next,..Mock::default()});
  let driver = Driver { profile: Profile { nonce_session: true }, sensor_power_ns: AtomicU64::new(0) };
  assert_eq!(driver.wake_sensor().is_some(),ready);
  MOCK.with(|m| {let m=m.borrow();assert_eq!(m.reads,2);assert!(m.registered);assert_eq!(m.patch,patch);});
 }
 // Without a nonce session the status is read after registration too, and
 // its acknowledgement alone decides: state 9 does not ask for the patch.
 for (next,ready,patch) in [
  (Some(sensor::Status{state:9,ack:0x29,silent:false}),true,None),
  (Some(sensor::Status{state:0,ack:0,silent:false}),true,Some(0)),
  (None,false,None),
 ] {
  MOCK.with(|m| *m.borrow_mut()=Mock{next,..Mock::default()});
  let driver = Driver::new();
  assert_eq!(driver.wake_sensor().is_some(),ready);
  MOCK.with(|m| {let m=m.borrow();assert_eq!(m.reads,2);assert!(m.registered);assert_eq!(m.patch,patch);});
 }
 // A suspend that begins once register_sensor() has returned stops the
 // attempt at the check after it: no post-registration status read and no
 // patch. The mock's register_sensor() stands for all of registration; the
 // real one, on a nonce-session profile, has already authenticated the
 // sensor and set its session up by then, so this models no stop before
 // the session.
 for nonce_session in [false, true] {
  MOCK.with(|m| *m.borrow_mut()=Mock{next:Some(sensor::Status{state:0,ack:0,silent:false}),suspend:true,..Mock::default()});
  let driver = Driver { profile: Profile { nonce_session }, sensor_power_ns: AtomicU64::new(0) };
  assert!(driver.wake_sensor().is_none());
  MOCK.with(|m| {let m=m.borrow();assert_eq!(m.reads,1);assert!(m.registered);assert_eq!(m.patch,None);});
 }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'test.rs').write_text(program)
            subprocess.run(['rustc', '--edition=2021', str(root / 'test.rs'), '-o', str(root / 'test')], check=True)
            subprocess.run([str(root / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
