echo Insert with 1 index
./insbench -c "dbname=postgres host=localhost port=5432 sslmode=disable" -x 0
echo Insert with 9 indexex
./insbench -c "dbname=postgres host=localhost port=5432 sslmode=disable" -x 8
echo Insert with 9 partial indexes
./insbench -c "dbname=postgres host=localhost port=5432 sslmode=disable" -x 8 -u 1
