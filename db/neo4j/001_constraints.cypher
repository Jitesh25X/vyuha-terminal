// Neo4j 5 constraints and indexes for the Vyuha knowledge graph.
// Node keys use our Postgres ids where they exist so the two stores join cleanly.

CREATE CONSTRAINT company_id     IF NOT EXISTS FOR (n:Company)       REQUIRE n.isin IS UNIQUE;
CREATE CONSTRAINT instrument_id  IF NOT EXISTS FOR (n:Instrument)    REQUIRE n.instrument_id IS UNIQUE;
CREATE CONSTRAINT sector_id      IF NOT EXISTS FOR (n:Sector)        REQUIRE n.name IS UNIQUE;
CREATE CONSTRAINT industry_id    IF NOT EXISTS FOR (n:Industry)      REQUIRE n.name IS UNIQUE;
CREATE CONSTRAINT index_id       IF NOT EXISTS FOR (n:Index)         REQUIRE n.symbol IS UNIQUE;
CREATE CONSTRAINT person_id      IF NOT EXISTS FOR (n:Person)        REQUIRE n.person_id IS UNIQUE;
CREATE CONSTRAINT event_id       IF NOT EXISTS FOR (n:Event)         REQUIRE n.event_id IS UNIQUE;
CREATE CONSTRAINT story_id       IF NOT EXISTS FOR (n:Story)         REQUIRE n.story_id IS UNIQUE;
CREATE CONSTRAINT macro_id       IF NOT EXISTS FOR (n:MacroVariable) REQUIRE n.code IS UNIQUE;
CREATE CONSTRAINT commodity_id   IF NOT EXISTS FOR (n:Commodity)     REQUIRE n.code IS UNIQUE;
CREATE CONSTRAINT trader_id      IF NOT EXISTS FOR (n:Trader)        REQUIRE n.user_id IS UNIQUE;
CREATE CONSTRAINT account_id     IF NOT EXISTS FOR (n:Account)       REQUIRE n.account_id IS UNIQUE;
CREATE CONSTRAINT strategy_id    IF NOT EXISTS FOR (n:Strategy)      REQUIRE n.strategy_id IS UNIQUE;

CREATE FULLTEXT INDEX entity_names IF NOT EXISTS
  FOR (n:Company|Person|Sector|Industry|Index|MacroVariable|Commodity) ON EACH [n.name, n.aliases_text];

CREATE INDEX story_published IF NOT EXISTS FOR (s:Story) ON (s.published_at);
CREATE INDEX event_type      IF NOT EXISTS FOR (e:Event) ON (e.type, e.date);
