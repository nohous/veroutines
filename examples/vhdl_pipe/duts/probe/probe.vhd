-- probe: a deliberately small DUT that carries one instance of each structure
-- whose survival through ghdl-synth -> verilog -> verilator we want to measure:
--   FF        : cnt   (a register, drives output count)
--   comb net  : par   (a xor b, drives output parity; carries a keep attribute)
--   record    : bus_s (valid/ready, internal, feeds the sub-instance)
--   hierarchy : u_leaf (a child entity with its own FF)
-- Each structure fans out to something observable so synthesis cannot delete it
-- for being dead; the question the pipe answers is whether the NAME and the
-- boundary survive, not whether the logic does.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

-- ----------------------------------------------------------------------------
-- probe_leaf: a child entity, present only to create a hierarchy boundary
-- ----------------------------------------------------------------------------
entity probe_leaf is
  port (
    clk : in  std_logic;
    d   : in  std_logic;
    q   : out std_logic
  );
end entity;

architecture rtl of probe_leaf is
  signal r : std_logic := '0';
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      r <= d;
    end if;
  end process;

  q <= r;
end architecture;

-- ----------------------------------------------------------------------------
-- probe: the top
-- ----------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity probe is
  port (
    clk    : in  std_logic;
    rst    : in  std_logic;
    a      : in  std_logic;
    b      : in  std_logic;
    count  : out std_logic_vector(7 downto 0);
    parity : out std_logic;
    leafq  : out std_logic
  );
end entity;

architecture rtl of probe is
  type bus_t is record
    valid : std_logic;
    ready : std_logic;
  end record;

  signal bus_s : bus_t;
  signal cnt   : unsigned(7 downto 0) := (others => '0');
  signal par   : std_logic;

  -- Vivado-style hint; the pipe reports whether ghdl-synth honors it.
  attribute keep : string;
  attribute keep of par : signal is "true";
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      if rst = '1' then
        cnt <= (others => '0');
      else
        cnt <= cnt + 1;
      end if;
    end if;
  end process;

  par <= a xor b;

  bus_s.valid <= a and (not rst);
  bus_s.ready <= b;

  u_leaf : entity work.probe_leaf
    port map (
      clk => clk,
      d   => bus_s.valid and bus_s.ready,
      q   => leafq
    );

  count  <= std_logic_vector(cnt);
  parity <= par;
end architecture;
