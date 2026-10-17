package br.com.ideiarec.companion.protocol;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertThrows;
import org.junit.Test;

public final class DeviceAddressTest {
    @Test public void normalizesCdmMacAddressAndLegacySavedAddress() {
        assertEquals("70:04:1D:D7:AE:A5", DeviceAddress.normalize("70:04:1d:d7:ae:a5"));
        assertEquals("70:04:1D:D7:AE:A5", DeviceAddress.normalize("70:04:1D:D7:AE:A5"));
        assertEquals("00:01:02:0A:0B:0C", DeviceAddress.normalize(" 00:01:02:0a:0b:0c "));
    }
    @Test public void rejectsNonCanonicalLengthSeparatorsAndUnicode() {
        for (String invalid : new String[]{"", "70:04:1d:d7:ae", "70041dd7aea5", "70-04-1d-d7-ae-a5",
                "70:04:1d:d7:ae:zz", "70:04:1d:d7:ae:a5:ff", "７０:04:1d:d7:ae:a5"}) {
            assertThrows(IllegalArgumentException.class, () -> DeviceAddress.normalize(invalid));
        }
        assertThrows(IllegalArgumentException.class, () -> DeviceAddress.normalize(null));
    }
}
