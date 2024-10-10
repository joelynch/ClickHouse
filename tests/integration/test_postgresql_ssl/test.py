import logging
import pytest
from multiprocessing.dummy import Pool

from helpers.cluster import ClickHouseCluster
from helpers.postgres_utility import get_postgres_conn

cluster = ClickHouseCluster(__file__)
node1 = cluster.add_instance(
    "node1",
    main_configs=["configs/named_collections.xml"],
    with_postgres_ssl=True,
)

@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster.start()
        assert cluster.postgresql_ssl_conn is not None
        cursor = cluster.postgresql_ssl_conn.cursor()
        cursor.execute("CREATE TABLE test_table(x int PRIMARY KEY)")
        cursor.execute("INSERT INTO test_table(x) VALUES (1), (2), (3)")
        cursor.close()
        yield cluster
    finally:
        cluster.shutdown()

def test_ssl_works(started_cluster: ClickHouseCluster) -> None:
    node1.query(
        """
        CREATE TABLE postgres_table (x Int32)
        ENGINE = PostgreSQL(`postgresql_ssl`)
        """
    )
    assert node1.query("SELECT count() FROM postgres_table") == "3\n"


def test_no_ssl_fails(started_cluster: ClickHouseCluster) -> None:
    with pytest.raises(Exception) as e:
        node1.query(
            """
            CREATE TABLE postgres_table_no_ssl (x Int32)
            ENGINE = PostgreSQL(`postgresql_no_ssl`)
            """
        )
