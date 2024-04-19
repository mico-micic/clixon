#!/usr/bin/env bash
# SNMP "smoketest" Basic snmpget test for non-existent values

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Re-use main example backend state callbacks
APPNAME=example

if [ ${ENABLE_NETSNMP} != "yes" ]; then
    echo "Skipping test, Net-SNMP support not enabled."
    rm -rf $dir
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

cfg=$dir/conf_startup.xml
fyang=$dir/clixon-example.yang
fstate=$dir/state.xml

# AgentX unix socket
SOCK=/var/run/snmp.sock

# Relies on example_backend.so for $fstate file handling

cat <<EOF > $cfg
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_YANG_DIR>${YANG_INSTALLDIR}</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>${YANG_STANDARD_DIR}</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>${MIB_GENERATED_YANG_DIR}</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_FILE>$fyang</CLICON_YANG_MAIN_FILE>
  <CLICON_SOCK>$dir/$APPNAME.sock</CLICON_SOCK>
  <CLICON_BACKEND_DIR>/usr/local/lib/$APPNAME/backend</CLICON_BACKEND_DIR>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/run/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_SNMP_AGENT_SOCK>unix:$SOCK</CLICON_SNMP_AGENT_SOCK>
  <CLICON_SNMP_MIB>CLIXON-TYPES-MIB</CLICON_SNMP_MIB>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
</clixon-config>
EOF

cat <<EOF > $fyang
module clixon-example{
  yang-version 1.1;
  namespace "urn:example:clixon";
  prefix ex;
  import CLIXON-TYPES-MIB {
      prefix "clixon-types";
  }
}
EOF

# This is state data written to file that backend reads from (on request)
# integer and string have values, sleeper does not and uses default (=1)

cat <<EOF > $fstate
<CLIXON-TYPES-MIB xmlns="urn:ietf:params:xml:ns:yang:smiv2:CLIXON-TYPES-MIB">
  <clixonExampleScalars>
    <clixonExampleString></clixonExampleString>
    <bitTest></bitTest>
    <clixonExampleStringNoDefault></clixonExampleStringNoDefault>
  </clixonExampleScalars>
</CLIXON-TYPES-MIB>
EOF

function testinit(){
    new "test params: -s init -f $cfg -- -sS $fstate"
    if [ $BE -ne 0 ]; then
        # Kill old backend and start a new one
        new "kill old backend"
        sudo clixon_backend -zf $cfg
        if [ $? -ne 0 ]; then
            err "Failed to start backend"
        fi

        sudo pkill -f clixon_backend

        new "Starting backend"
        start_backend -s init -f $cfg -- -sS $fstate
    fi

    new "wait backend"
    wait_backend

    if [ $SN -ne 0 ]; then
        # Kill old clixon_snmp, if any
        new "Terminating any old clixon_snmp processes"
        sudo killall -q clixon_snmp

        new "Starting clixon_snmp"
        start_snmp $cfg
    fi

    new "wait snmp"
    wait_snmp
}

function testexit(){
    stop_snmp
    if [ $BE -ne 0 ]; then
        new "Kill backend"
        # Check if premature kill
        pid=$(pgrep -u root -f clixon_backend)
        if [ -z "$pid" ]; then
            err "backend already dead"
        fi
        # kill backend
        stop_backend -f $cfg
    fi
}

new "SNMP tests"
testinit

MIB=".1.3.6.1.4.1.8072.200"
OID1="${MIB}.1.3.0"           # clixonExampleString
OID2="${MIB}.1.14.0"          # bitTest bit00(0) bit12(12) bit22(22) bit35(35)
OID3="${MIB}.1.15.0"          # clixonExampleStringNoDefault
OID_INVALID="${MIB}.50.50.0"  # something that does not exist

NAME1="CLIXON-TYPES-MIB::clixonExampleString.0"
NAME2="CLIXON-TYPES-MIB::bitTest.0"
NAME3="CLIXON-TYPES-MIB::clixonExampleStringNoDefault.0"
NAME_INVALID="CLIXON-TYPES-MIB::clixonExampleStringNoDefault.1"  # something that does not exist

new "$snmpget"

new "Get netSnmpExampleString"
validate_oid $OID1 $OID1 "STRING" "\"So long, and thanks for all the fish!\""
validate_oid $NAME1 $NAME1 "STRING" "So long, and thanks for all the fish!"

new "Get emptyBitTest"
validate_oid $OID2 $OID2 "Hex-STRING" "" "\"\""
validate_oid $NAME2 $NAME2 "BITS" "" ""

new "Get clixonExampleStringNoDefault"
validate_oid $OID3 $OID3 "STRING" "" "\"\""
validate_oid $NAME3 $NAME3 "STRING" "" ""

new "Get noSuchObject"
validate_oid $OID_INVALID $OID_INVALID "" "" "No Such Object available on this agent at this OID"
validate_oid $NAME_INVALID $NAME_INVALID "" "" "No Such Object available on this agent at this OID"

new "Cleaning up"
testexit

rm -rf $dir

new "endtest"
endtest
