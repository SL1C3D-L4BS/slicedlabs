# engine/fabric/testing

`sl_fabric_testing` (alias `sl::fabric_testing`): helpers shared by the fabric tests (WO-037 created it). Every
`testing/*.cpp` is globbed into the library; `testing_base.cpp` keeps it non-empty. Helper headers go in
`testing/include/` (already on the include path). Every fabric test links it. Later test work orders add files only.
