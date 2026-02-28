-- test_antenna_link.vhd
-- RF source -> TX antenna -> [air net / Friis coupling] -> RX antenna
--
-- Tests antenna arena resolution with Friis free-space path loss.
--
-- Setup: 1 mW TX, 0 dBi isotropic antennas, 10m distance, 2.4 GHz
--
-- Friis equation (power):
--   P_rx = P_tx * G_tx * G_rx * (lambda / (4*pi*d))^2
--   lambda = c / f = 299792458 / 2.4e9 = 0.12491 m
--   FSPL = (lambda / (4*pi*d))^2 = (0.12491 / 125.664)^2 = 9.878e-7
--   P_rx = 1e-3 * 1 * 1 * 9.878e-7 = 9.878e-10 W
--
-- The antenna entities apply sqrt(gain) on each side, so each antenna
-- contributes a factor of 1.0 (0 dBi). The channel component applies
-- the FSPL amplitude factor.
--
-- Architecture: source -> antenna_tx.feed, antenna_tx.air -> channel.a,
--               channel.b -> antenna_rx.air, antenna_rx.feed (leaf)
-- The channel entity handles the Friis path loss between antennas.

-- DUT: only std_logic nets
library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
library rf;

entity test_antenna_link is end entity;

architecture test of test_antenna_link is
    signal n_feed_tx : std_logic;
    signal n_air_tx  : std_logic;
    signal n_air_rx  : std_logic;
    signal n_feed_rx : std_logic;
begin
    src1: entity rf.rf_source(behavioral)
        generic map (
            power_w   => 1.0e-3,
            freq      => 2.4e9,
            pol_angle => 0.0
        )
        port map (output => n_feed_tx);

    ant_tx: entity rf.rf_antenna(behavioral)
        generic map (gain_dbi => 0.0, pol_angle => 0.0)
        port map (feed => n_feed_tx, air => n_air_tx);

    chan1: entity rf.rf_channel(behavioral)
        generic map (distance => 10.0)
        port map (a => n_air_tx, b => n_air_rx);

    ant_rx: entity rf.rf_antenna(behavioral)
        generic map (gain_dbi => 0.0, pol_angle => 0.0)
        port map (feed => n_feed_rx, air => n_air_rx);
end architecture;

-- Hand-written resolver + test assertions
library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
library rf;
use rf.rf_signal_pkg.all;

entity rn_test_antenna_link is end entity;

architecture hand_written of rn_test_antenna_link is

    -- Net n_feed_tx: src1.output <-> ant_tx.feed
    alias drv_src     is << signal .resolved_test_antenna_link.dut.src1.output.driver : rf_signal >>;
    alias oth_src     is << signal .resolved_test_antenna_link.dut.src1.output.other : rf_signal >>;
    alias drv_tx_feed is << signal .resolved_test_antenna_link.dut.ant_tx.feed.driver : rf_signal >>;
    alias oth_tx_feed is << signal .resolved_test_antenna_link.dut.ant_tx.feed.other : rf_signal >>;

    -- Net n_air_tx: ant_tx.air <-> chan1.a
    alias drv_tx_air  is << signal .resolved_test_antenna_link.dut.ant_tx.air.driver : rf_signal >>;
    alias oth_tx_air  is << signal .resolved_test_antenna_link.dut.ant_tx.air.other : rf_signal >>;
    alias drv_chan_a   is << signal .resolved_test_antenna_link.dut.chan1.a.driver : rf_signal >>;
    alias oth_chan_a   is << signal .resolved_test_antenna_link.dut.chan1.a.other : rf_signal >>;

    -- Net n_air_rx: chan1.b <-> ant_rx.air
    alias drv_chan_b   is << signal .resolved_test_antenna_link.dut.chan1.b.driver : rf_signal >>;
    alias oth_chan_b   is << signal .resolved_test_antenna_link.dut.chan1.b.other : rf_signal >>;
    alias drv_rx_air  is << signal .resolved_test_antenna_link.dut.ant_rx.air.driver : rf_signal >>;
    alias oth_rx_air  is << signal .resolved_test_antenna_link.dut.ant_rx.air.other : rf_signal >>;

    -- Net n_feed_rx: ant_rx.feed (leaf)
    alias drv_rx_feed is << signal .resolved_test_antenna_link.dut.ant_rx.feed.driver : rf_signal >>;
    alias oth_rx_feed is << signal .resolved_test_antenna_link.dut.ant_rx.feed.other : rf_signal >>;

begin
    -- Net n_feed_tx: source <-> TX antenna feed (swap)
    p_feed_tx: process(drv_src, drv_tx_feed)
    begin
        oth_src     := drv_tx_feed;
        oth_tx_feed := drv_src;
    end process;

    -- Net n_air_tx: TX antenna air <-> channel a (swap)
    p_air_tx: process(drv_tx_air, drv_chan_a)
    begin
        oth_tx_air := drv_chan_a;
        oth_chan_a  := drv_tx_air;
    end process;

    -- Net n_air_rx: channel b <-> RX antenna air (swap)
    p_air_rx: process(drv_chan_b, drv_rx_air)
    begin
        oth_chan_b  := drv_rx_air;
        oth_rx_air := drv_chan_b;
    end process;

    -- Leaf: RX antenna feed (no other drivers)
    p_leaf: process
    begin
        oth_rx_feed := RF_ZERO;
        wait;
    end process;

    -- Test assertions
    p_check: process
        variable p_tx      : real;
        variable p_rx      : real;
        variable wavelength: real;
        variable fspl_power: real;
        variable expected  : real;
        variable tol       : real;
    begin
        wait for 10 ns;

        -- TX power at source driver
        p_tx := rf_power(drv_src);

        -- RX power at receive antenna feed driver
        p_rx := rf_power(drv_rx_feed);

        -- Expected: Friis equation
        -- P_rx = P_tx * G_tx * G_rx * (lambda / 4*pi*d)^2
        -- With 0 dBi antennas, G_tx = G_rx = 1
        -- But the antenna applies sqrt(G) amplitude on each side (TX and RX)
        -- so the antenna gain is already included.
        -- The channel applies FSPL amplitude = lambda / (4*pi*d)
        -- Total: P_rx = P_tx * (lambda / (4*pi*d))^2
        wavelength := SPEED_OF_LIGHT / 2.4e9;
        fspl_power := (wavelength / (4.0 * MATH_PI * 10.0)) ** 2;
        expected := 1.0e-3 * fspl_power;
        tol := expected * 0.01;  -- 1% tolerance

        report "=== Antenna Link Test ===" severity note;
        report "TX power:       " & real'image(p_tx) & " W" severity note;
        report "RX power:       " & real'image(p_rx) & " W" severity note;
        report "Expected RX:    " & real'image(expected) & " W" severity note;
        report "FSPL (dB):      " & real'image(10.0 * log10(fspl_power)) severity note;
        report "RX power (dBm): " & real'image(watts_to_dbm(p_rx)) severity note;

        assert abs(p_tx - 1.0e-3) < 1.0e-9
            report "FAIL: TX power " & real'image(p_tx)
            severity error;

        assert abs(p_rx - expected) < tol
            report "FAIL: RX power " & real'image(p_rx) &
                   " expected " & real'image(expected)
            severity error;

        report "DONE" severity note;
        wait;
    end process;

end architecture;

-- Wrapper
entity resolved_test_antenna_link is end entity;

architecture wrap of resolved_test_antenna_link is
begin
    dut:      entity work.test_antenna_link;
    resolver: entity work.rn_test_antenna_link;
end architecture;
