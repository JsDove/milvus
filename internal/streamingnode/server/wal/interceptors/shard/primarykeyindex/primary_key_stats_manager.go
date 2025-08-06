package primaryindex

import (
	"context"
	"sync"

	"github.com/milvus-io/milvus-proto/go-api/v2/msgpb"
	"github.com/milvus-io/milvus-proto/go-api/v2/schemapb"
	"github.com/milvus-io/milvus/internal/storage"
	"github.com/milvus-io/milvus/internal/util/indexcgowrapper"
	"github.com/milvus-io/milvus/pkg/v2/common"
	"github.com/milvus-io/milvus/pkg/v2/proto/datapb"
	"github.com/milvus-io/milvus/pkg/v2/proto/indexcgopb"

	"github.com/milvus-io/milvus/pkg/v2/log"
	"github.com/pkg/errors"
	"go.uber.org/zap"
)

type PKStatsManager struct {
	growingSegmentPK  map[string]map[int64]*storage.PrimaryKeyStats
	pkFieldID         int64
	pkType            int64
	sealedIndexHandle map[string]*indexcgowrapper.PrimaryIndexHandle
	mu                sync.RWMutex
}

func NewPKStatsManager() *PKStatsManager {
	return &PKStatsManager{
		growingSegmentPK:  make(map[string]map[int64]*storage.PrimaryKeyStats),
		sealedIndexHandle: make(map[string]*indexcgowrapper.PrimaryIndexHandle),
		pkFieldID:         0,
		pkType:            0,
	}
}

func (gspm *PKStatsManager) createSegmentStats(vchannelName string, segmentID int64, pkFieldID int64, pkType int64) *storage.PrimaryKeyStats {
	if gspm.growingSegmentPK[vchannelName] == nil {
		gspm.growingSegmentPK[vchannelName] = make(map[int64]*storage.PrimaryKeyStats)
	}

	stats, err := storage.NewPrimaryKeyStats(pkFieldID, pkType, 200000)
	if err != nil {
		log.Error("failed to create primary key stats for segment",
			zap.Int64("segmentID", segmentID),
			zap.Error(err))
		return nil
	}

	gspm.growingSegmentPK[vchannelName][segmentID] = stats
	log.Info("created primary key stats for growing segment",
		zap.Int64("segmentID", segmentID),
		zap.Int64("pkFieldID", pkFieldID))

	return stats
}

func (gspm *PKStatsManager) UpdatePrimaryKey(vchannelName string, segmentID int64, pk storage.PrimaryKey) error {
	gspm.mu.Lock()
	defer gspm.mu.Unlock()

	stats, exists := gspm.growingSegmentPK[vchannelName][segmentID]
	if !exists {
		stats = gspm.createSegmentStats(vchannelName, segmentID, gspm.pkFieldID, gspm.pkType)
		if stats == nil {
			return errors.New("failed to create primary key stats for segment")
		}
	}

	stats.Update(pk)
	return nil
}

func (gspm *PKStatsManager) BatchUpdatePrimaryKeys(vchannelName string, segmentID int64, pks []storage.PrimaryKey) error {
	gspm.mu.Lock()
	defer gspm.mu.Unlock()

	stats, exists := gspm.growingSegmentPK[vchannelName][segmentID]
	if !exists {
		stats = gspm.createSegmentStats(vchannelName, segmentID, gspm.pkFieldID, gspm.pkType)
		if stats == nil {
			return errors.New("failed to create primary key stats for segment")
		}
	}

	for _, pk := range pks {
		stats.Update(pk)
	}

	log.Debug("batch updated primary key stats for segment",
		zap.Int64("segmentID", segmentID),
		zap.Int("count", len(pks)))

	return nil
}

func (gspm *PKStatsManager) RemoveSegmentStats(vchannelName string, segmentID int64) {
	gspm.mu.Lock()
	defer gspm.mu.Unlock()

	if _, exists := gspm.growingSegmentPK[vchannelName][segmentID]; exists {
		delete(gspm.growingSegmentPK[vchannelName], segmentID)
		log.Info("removed primary key stats for segment", zap.Int64("segmentID", segmentID))
	}
}

func (gspm *PKStatsManager) Close() {
	gspm.mu.Lock()
	defer gspm.mu.Unlock()

	for _, handle := range gspm.sealedIndexHandle {
		handle.Close()
	}

	gspm.growingSegmentPK = nil
}

func (gspm *PKStatsManager) CheckDuplicatePrimaryKeys(vchannelName string, pks []storage.PrimaryKey) (*schemapb.IDs, []int64) {
	gspm.mu.RLock()
	defer gspm.mu.RUnlock()
	var duplicates []storage.PrimaryKey
	var segmentIDs []int64
	for _, pk := range pks {
		for segmentID, stats := range gspm.growingSegmentPK[vchannelName] {
			if stats == nil {
				continue
			}
			if stats.BF != nil {
				var exists bool
				switch pk.Type() {
				case schemapb.DataType_Int64:
					buf := make([]byte, 8)
					int64Value := pk.(*storage.Int64PrimaryKey).Value
					common.Endian.PutUint64(buf, uint64(int64Value))
					exists = stats.BF.Test(buf)
				case schemapb.DataType_VarChar:
					stringValue := pk.(*storage.VarCharPrimaryKey).Value
					exists = stats.BF.TestString(stringValue)
				}
				if exists {
					log.Debug("check duplicate primary keys", zap.Any("pk", pk), zap.Int64("segmentID", segmentID))
					duplicates = append(duplicates, pk)
					segmentIDs = append(segmentIDs, segmentID)
					continue
				}
			}
		}
		if gspm.sealedIndexHandle[vchannelName] != nil {
			result, err := gspm.sealedIndexHandle[vchannelName].Query(pk.(*storage.VarCharPrimaryKey).Value)
			if err != nil {
				log.Error("failed to query sealed index", zap.Error(err))
				continue
			}
			log.Debug("query sealed index", zap.Int64("result", result))
			if result != -1 {
				duplicates = append(duplicates, pk)
				segmentIDs = append(segmentIDs, result)
			}
		}
	}
	ids := storage.ParsePrimaryKeys2IDs(duplicates)
	return ids, segmentIDs
}

func (gspm *PKStatsManager) SetPrimaryKeyInfo(pkFieldID int64, pkType int64) {
	gspm.mu.Lock()
	defer gspm.mu.Unlock()
	gspm.pkFieldID = pkFieldID
	gspm.pkType = pkType
	log.Info("set primary key info", zap.Int64("pkFieldID", pkFieldID), zap.Int64("pkType", pkType))
}

func (gspm *PKStatsManager) GetPrimaryKeyInfo() (int64, int64) {
	gspm.mu.RLock()
	defer gspm.mu.RUnlock()
	return gspm.pkFieldID, gspm.pkType
}

func (gspm *PKStatsManager) convertToPrimaryKeys(data []interface{}) []storage.PrimaryKey {
	pks := make([]storage.PrimaryKey, len(data))
	for i, v := range data {
		switch val := v.(type) {
		case int64:
			pks[i] = storage.NewInt64PrimaryKey(val)
		case string:
			pks[i] = storage.NewVarCharPrimaryKey(val)
		default:
			log.Warn("unsupported primary key type", zap.Any("value", v))
		}
	}
	return pks
}

func (gspm *PKStatsManager) ExtractColumnData(insertMsg interface{}, fieldID int64) ([]interface{}, error) {
	msg, ok := insertMsg.(interface {
		Body() (*msgpb.InsertRequest, error)
	})
	if !ok {
		return nil, errors.New("invalid insert message type")
	}

	body, err := msg.Body()
	if err != nil {
		return nil, err
	}

	var fieldData *schemapb.FieldData
	for _, fd := range body.FieldsData {
		if fd.FieldId == fieldID {
			fieldData = fd
			break
		}
	}

	if fieldData == nil {
		return nil, errors.New("field not found")
	}

	switch fieldData.Type {
	case schemapb.DataType_Int64:
		if intData := fieldData.GetScalars().GetIntData(); intData != nil {
			result := make([]interface{}, len(intData.Data))
			for i, value := range intData.Data {
				result[i] = int64(value)
			}
			return result, nil
		}
	case schemapb.DataType_VarChar:
		if stringData := fieldData.GetScalars().GetStringData(); stringData != nil {
			result := make([]interface{}, len(stringData.Data))
			for i, value := range stringData.Data {
				result[i] = value
			}
			return result, nil
		}
	default:
		return nil, errors.Errorf("unsupported data type: %s", fieldData.Type.String())
	}

	return nil, errors.New("no data found for field")
}

func (gspm *PKStatsManager) ExtractPrimaryKeyColumn(insertMsg interface{}) ([]storage.PrimaryKey, error) {
	pkFieldID, _ := gspm.GetPrimaryKeyInfo()
	if pkFieldID == 0 {
		return nil, errors.New("primary key field ID not set")
	}

	data, err := gspm.ExtractColumnData(insertMsg, pkFieldID)
	if err != nil {
		log.Warn("failed to extract primary key column",
			zap.Int64("fieldID", pkFieldID),
			zap.Error(err))
		return nil, err
	}

	log.Debug("extracted primary key column data",
		zap.Int64("fieldID", pkFieldID),
		zap.Int("count", len(data)))

	return gspm.convertToPrimaryKeys(data), nil
}

func (gspm *PKStatsManager) UpdateBloomFilterFromPrimaryKeys(vchannelName string, primaryKeys []storage.PrimaryKey, segmentID int64) {
	if err := gspm.BatchUpdatePrimaryKeys(vchannelName, segmentID, primaryKeys); err != nil {
		log.Warn("failed to update primary key stats for segment",
			zap.Int64("segmentID", segmentID),
			zap.Int("pkCount", len(primaryKeys)),
			zap.Error(err))
	}
}

func (gspm *PKStatsManager) LoadSealedIndex(eventData *datapb.PrimaryKeyIndexBuiltData) {
	gspm.mu.Lock()
	defer gspm.mu.Unlock()
	log.Info("loading sealed index", zap.String("vchannelName", eventData.VchannelName), zap.Int64("version", eventData.Version), zap.Int64("collectionID", eventData.CollectionId), zap.Strings("files", eventData.Files))
	if gspm.sealedIndexHandle[eventData.VchannelName] != nil {
		gspm.sealedIndexHandle[eventData.VchannelName].Close()
	}

	loadParams := &indexcgopb.LoadPrimaryIndexInfo{
		BuildID:      eventData.Version,
		CollectionID: eventData.CollectionId,
		Files:        eventData.Files,
	}

	handle, err := indexcgowrapper.LoadPrimaryIndex(context.Background(), loadParams)
	if err != nil {
		log.Error("failed to load sealed segment index", zap.Error(err))
		return
	}

	gspm.sealedIndexHandle[eventData.VchannelName] = handle

	segmentList, err := handle.GetSegmentList()
	if err != nil {
		log.Error("failed to get segment list", zap.Error(err))
		return
	}
	for _, segmentID := range segmentList {
		if _, exists := gspm.growingSegmentPK[eventData.VchannelName][segmentID]; exists {
			log.Info("delete growing segment pk", zap.Int64("segmentID", segmentID), zap.String("vchannelName", eventData.VchannelName), zap.Int("length", len(gspm.growingSegmentPK[eventData.VchannelName])))
			delete(gspm.growingSegmentPK[eventData.VchannelName], segmentID)
			log.Info("after delete growing segment pk", zap.Int64("segmentID", segmentID), zap.String("vchannelName", eventData.VchannelName), zap.Int("length", len(gspm.growingSegmentPK[eventData.VchannelName])), zap.Int("length", len(gspm.growingSegmentPK[eventData.VchannelName])))
		}
	}

	log.Info("loaded sealed segment index",
		zap.Int64("collectionID", eventData.CollectionId),
		zap.String("vchannel", eventData.VchannelName),
		zap.Strings("files", eventData.Files))
}

func (gspm *PKStatsManager) SegmentChange(eventData *datapb.SegmentCompactionData) {
	gspm.mu.Lock()
	defer gspm.mu.Unlock()
	for _, seg := range eventData.Infos {
		if gspm.growingSegmentPK[seg.GetInsertChannel()] != nil {
			for _, segmentIDfrom := range seg.GetCompactionFrom() {
				log.Info("segment change", zap.Int64("segmentID", seg.GetID()), zap.Int64("segmentIDfrom", segmentIDfrom), zap.String("vchannelName", seg.GetInsertChannel()),
					zap.Int("length", len(gspm.growingSegmentPK[seg.GetInsertChannel()])))
				if _, exists := gspm.growingSegmentPK[seg.GetInsertChannel()][segmentIDfrom]; exists {
					gspm.growingSegmentPK[seg.GetInsertChannel()][seg.GetID()] = gspm.growingSegmentPK[seg.GetInsertChannel()][segmentIDfrom]
					delete(gspm.growingSegmentPK[seg.GetInsertChannel()], segmentIDfrom)
				}
				log.Info("after delete segment change", zap.Int64("segmentID", seg.GetID()), zap.Int64("segmentIDfrom", segmentIDfrom), zap.String("vchannelName", seg.GetInsertChannel()),
					zap.Int("length", len(gspm.growingSegmentPK[seg.GetInsertChannel()])))
			}
		}
		if gspm.sealedIndexHandle[seg.GetInsertChannel()] != nil {
			for _, segmentIDfrom := range seg.GetCompactionFrom() {
				gspm.sealedIndexHandle[seg.GetInsertChannel()].ResetSegmentId(seg.GetID(), segmentIDfrom)
			}
		}
		log.Info("growingSegmentPK change done")
	}
}
