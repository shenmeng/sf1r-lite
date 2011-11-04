#include "product_manager.h"
#include "product_data_source.h"
#include "operation_processor.h"
#include "uuid_generator.h"
#include "product_backup.h"
#include <boost/unordered_set.hpp>

using namespace sf1r;

ProductManager::ProductManager(ProductDataSource* data_source, OperationProcessor* op_processor, const PMConfig& config)
:data_source_(data_source), op_processor_(op_processor), uuid_gen_(new UuidGenerator()), backup_(NULL), config_(config), inhook_(false)
{
    if(config_.backup_path!="")
    {
        backup_ = new ProductBackup(config_.backup_path);
    }
}

ProductManager::~ProductManager()
{
    delete uuid_gen_;
}

bool ProductManager::Recover()
{
    if(backup_==NULL)
    {
        error_ = "Backup not initialzed.";
        return false;
    }
    if(!backup_->Recover(this))
    {
        error_ = "Backup recover failed.";
        return false;
    }
    return true;
}


bool ProductManager::HookInsert(PMDocumentType& doc, izenelib::ir::indexmanager::IndexerDocument& index_document)
{
    inhook_ = true;
    boost::mutex::scoped_lock lock(human_mutex_);
    izenelib::util::UString uuid = uuid_gen_->Gen(doc);
    if(!data_source_->SetUuid(index_document, uuid)) return false;
    PMDocumentType new_doc(doc);
    doc.property(config_.uuid_property_name) = uuid;
    new_doc.property(config_.docid_property_name) = uuid;
    SetItemCount_(new_doc, 1);
    op_processor_->Append(1, new_doc);
    return true;
}

bool ProductManager::HookUpdate(PMDocumentType& to, izenelib::ir::indexmanager::IndexerDocument& index_document, bool r_type)
{
    inhook_ = true;
    boost::mutex::scoped_lock lock(human_mutex_);
    uint32_t fromid = index_document.getId(); //oldid
    PMDocumentType from;
    if(!data_source_->GetDocument(fromid, from)) return false;
    izenelib::util::UString from_uuid;
    if(!GetUuid_(from, from_uuid)) return false;
    std::vector<uint32_t> docid_list;
    data_source_->GetDocIdList(from_uuid, docid_list, fromid); // except from.docid
    if(docid_list.empty()) // the from doc is unique, so delete it and insert 'to'
    {
        if(!data_source_->SetUuid(index_document, from_uuid)) return false;
        PMDocumentType new_doc(to);
        to.property(config_.uuid_property_name) = from_uuid;
        new_doc.property(config_.docid_property_name) = from_uuid;
        SetItemCount_(new_doc, 1);
        op_processor_->Append( 2, new_doc);// if r_type, only numberic properties in 'to'
    }
    else
    {
        //need not to update(insert) to.uuid, 
        ProductPrice from_price;
        ProductPrice to_price;
        if(!GetPrice_(fromid, from_price)) return false;
        if(!GetPrice_(to, to_price)) return false;
        if(!data_source_->SetUuid(index_document, from_uuid)) return false;
        to.property(config_.uuid_property_name) = from_uuid;
        //update price only
        if(from_price!=to_price)
        {
            PMDocumentType diff_properties;
            ProductPrice price(to_price);
            GetPrice_(docid_list, price);
            diff_properties.property(config_.price_property_name) = price.ToUString();
            diff_properties.property(config_.docid_property_name) = from_uuid;
            //auto r_type? itemcount no need?
//                 diff_properties.property(config_.itemcount_property_name) = izenelib::util::UString(boost::lexical_cast<std::string>(docid_list.size()+1), izenelib::util::UString::UTF_8);
            
            op_processor_->Append( 2, diff_properties );
        }
    }
    return true;
}

bool ProductManager::HookDelete(uint32_t docid)
{
    inhook_ = true;
    boost::mutex::scoped_lock lock(human_mutex_);
    PMDocumentType from;
    if(!data_source_->GetDocument(docid, from)) return false;
    izenelib::util::UString from_uuid;
    if(!GetUuid_(from, from_uuid)) return false;
    std::vector<uint32_t> docid_list;
    data_source_->GetDocIdList(from_uuid, docid_list, docid); // except from.docid
    if(docid_list.empty()) // the from doc is unique, so delete it in A
    {
        PMDocumentType del_doc;
        del_doc.property(config_.docid_property_name) = from_uuid;
        op_processor_->Append(3, del_doc);
    }
    else
    {
        PMDocumentType diff_properties;
        ProductPrice price;
        GetPrice_(docid_list, price);
        diff_properties.property(config_.price_property_name) = price.ToUString();
        diff_properties.property(config_.docid_property_name) = from_uuid;
        SetItemCount_(diff_properties, docid_list.size());
        op_processor_->Append(2, diff_properties);
    }
    return true;
}

bool ProductManager::GenOperations()
{
    bool result = true;
    if(!op_processor_->Finish())
    {
        error_ = op_processor_->GetLastError();
        result = false;
    }
    if(inhook_) inhook_ = false;
    return result;

    /// M
    /*
    void ProductManager::onProcessed(bool success)
    {
        // action after SCDs have been processed
    }

    DistributedProcessSynchronizer dsSyn;
    std::string scdDir = "/tmp/hdfs/scd"; // generated scd files in scdDir
    dsSyn.generated(scdDir);
    dsSyn.watchProcess(boost::bind(&ProductManager::onProcessed, this,_1));
    */

    /// A
    /*
    class Receiver {
    public:
        Receiver()
        {
            dsSyn.watchGenerate(boost::bind(&Receiver::onGenerated, this,_1));
        }

        void Receiver::onGenerated(const std::string& s)
        {
            // process SCDs

            dsSyn.processed(true);
        }

        DistributedProcessSynchronizer dsSyn;
    }
    */
}


bool ProductManager::UpdateADoc(const Document& doc, bool backup)
{
    if(!UpdateADoc_(doc))
    {
        error_ = "Update A Doc failed";
        return false;
    }
    if(!GenOperations())
    {
        return false;
    }
    if(backup && backup_)
    {
        backup_->AddProductUpdateItem(doc);
    }
    return true;
}

bool ProductManager::UpdateADoc_(const Document& doc)
{
    op_processor_->Append(2, doc);
    return true;
}



bool ProductManager::AddGroupWithInfo(const std::vector<izenelib::util::UString>& docid_list, const Document& doc, bool backup)
{
    std::vector<uint32_t> idocid_list;
    if(!data_source_->GetInternalDocidList(docid_list, idocid_list))
    {
        error_ = data_source_->GetLastError();
        return false;
    }
    return AddGroupWithInfo(idocid_list, doc, backup);
}

void ProductManager::BackupPCItem_(const izenelib::util::UString& uuid, const std::vector<uint32_t>& docid_list, int type)
{
    std::vector<izenelib::util::UString> docname_list;
    for(uint32_t i=0;i<docid_list.size();i++)
    {
        PMDocumentType doc;
        if(data_source_->GetDocument(docid_list[i], doc))
        {
            docname_list.push_back(doc.property(config_.docid_property_name).get<izenelib::util::UString>());
        }
    }
    if(!backup_->AddPriceComparisonItem(uuid, docname_list, type))
    {
        std::cout<<"backup failed"<<std::endl;
    }
}

bool ProductManager::AddGroupWithInfo(const std::vector<uint32_t>& docid_list, const Document& doc, bool backup)
{
    if(inhook_)
    {
        error_ = "In Hook locks, collection was indexing, plz wait.";
        return false;
    }
    boost::mutex::scoped_lock lock(human_mutex_);
    std::cout<<"ProductManager::AddGroupWithInfo"<<std::endl;
    izenelib::util::UString uuid = doc.property(config_.docid_property_name).get<izenelib::util::UString>();
    if(uuid.length()==0)
    {
        error_ = "DOCID(uuid) not set";
        return false;
    }
    std::vector<uint32_t> uuid_docid_list;
    data_source_->GetDocIdList(uuid, uuid_docid_list, 0);
    if(!uuid_docid_list.empty())
    {
        std::string suuid;
        uuid.convertString(suuid, izenelib::util::UString::UTF_8);
        error_ = suuid+" already exists";
        return false;
    }
    //call updateA
    
    if(!AppendToGroup_(uuid, uuid_docid_list, docid_list, doc))
    {
        return false;
    }
    
    if(!GenOperations())
    {
        return false;
    }
    if(backup && backup_)
    {
        BackupPCItem_(uuid, docid_list, 1);
        backup_->AddProductUpdateItem(doc);
    }
    return true;
}

bool ProductManager::AddGroup(const std::vector<uint32_t>& docid_list, izenelib::util::UString& gen_uuid, bool backup)
{
    if(inhook_)
    {
        error_ = "In Hook locks, collection was indexing, plz wait.";
        return false;
    }
    boost::mutex::scoped_lock lock(human_mutex_);
    std::cout<<"ProductManager::AddGroup"<<std::endl;
    if(docid_list.size()<2) 
    {
        error_ = "Docid list size must larger than 1";
        return false;
    }
    PMDocumentType first_doc;
    if(!data_source_->GetDocument(docid_list[0], first_doc))
    {
        error_ = "Can not get document "+boost::lexical_cast<std::string>(docid_list[0]);
        return false;
    }
    izenelib::util::UString first_uuid;
    if(!GetUuid_(first_doc, first_uuid))
    {
        error_ = "Can not get uuid in document "+boost::lexical_cast<std::string>(docid_list[0]);
        return false;
    }
    std::vector<uint32_t> uuid_docid_list;
    data_source_->GetDocIdList(first_uuid, uuid_docid_list, 0);
    if(uuid_docid_list.empty())
    {
        std::string suuid;
        first_uuid.convertString(suuid, izenelib::util::UString::UTF_8);
        error_ = suuid+" not exists";
        return false;
    }
    if(uuid_docid_list.size()>1 || uuid_docid_list[0]!= docid_list[0] )
    {
        error_ = "Document id "+boost::lexical_cast<std::string>(docid_list[0])+" belongs to other group";
        return false;
    }

    std::vector<uint32_t> remain(docid_list.begin()+1, docid_list.end());
    if(!AppendToGroup_(first_uuid, uuid_docid_list, remain, PMDocumentType()))
    {
        return false;
    }
    if(!GenOperations())
    {
        return false;
    }
    if(backup && backup_)
    {
        BackupPCItem_(first_uuid, docid_list, 1);
    }
    gen_uuid = first_uuid;
    return true;
}

bool ProductManager::AppendToGroup_(const izenelib::util::UString& uuid, const std::vector<uint32_t>& uuid_docid_list, const std::vector<uint32_t>& docid_list, const PMDocumentType& uuid_doc)
{
    if(docid_list.empty()) 
    {
        error_ = "Docid list size must larger than 0";
        return false;
    }

    std::vector<PMDocumentType> doc_list(docid_list.size());
    for(uint32_t i=0;i<docid_list.size();i++)
    {
        if(!data_source_->GetDocument(docid_list[i], doc_list[i]))
        {
            error_ = "Can not get document "+boost::lexical_cast<std::string>(docid_list[i]);
            return false;
        }
    }
    std::vector<izenelib::util::UString> uuid_list(doc_list.size());
    for(uint32_t i=0;i<doc_list.size();i++)
    {
        if(!GetUuid_(doc_list[i], uuid_list[i]))
        {
            error_ = "Can not get uuid in document "+boost::lexical_cast<std::string>(docid_list[i]);
            return false;
        }
        std::vector<uint32_t> same_docid_list;
        data_source_->GetDocIdList(uuid_list[i], same_docid_list, docid_list[i]);
        if(!same_docid_list.empty())
        {
            error_ = "Document id "+boost::lexical_cast<std::string>(docid_list[i])+" belongs to other group";
            return false;
        }
        if(uuid_list[i] == uuid)
        {
            error_ = "Document id "+boost::lexical_cast<std::string>(docid_list[i])+" has the same uuid with request";
            return false;
        }
    }
    
    
    std::vector<uint32_t> all_docid_list(uuid_docid_list);
    all_docid_list.insert(all_docid_list.end(), docid_list.begin(), docid_list.end());
    ProductPrice price;
    GetPrice_(all_docid_list, price);
//     std::cout<<"validation finished here."<<std::endl;
    //validation finished here.
    
    
    
    //commit firstly, then update DM and IM
    for(uint32_t i=0;i<uuid_list.size();i++)
    {
        PMDocumentType del_doc;
        del_doc.property(config_.docid_property_name) = uuid_list[i];
        op_processor_->Append(3, del_doc);
    }
    if(uuid_docid_list.empty())
    {
        //this uuid is a new, use the first doc as base;
        PMDocumentType output(doc_list[0]);
        PMDocumentType::property_const_iterator uit = uuid_doc.propertyBegin();
        while( uit != uuid_doc.propertyEnd())
        {
            output.property(uit->first) = uit->second;
            ++uit;
        }
        output.property(config_.docid_property_name) = uuid;
        SetItemCount_(output, all_docid_list.size());
        output.property(config_.price_property_name) = price.ToUString();
        output.eraseProperty(config_.uuid_property_name);
        op_processor_->Append(1, output);
    }
    else
    {
        PMDocumentType output;
        if(uuid_doc.hasProperty(config_.docid_property_name))
        {
            output = uuid_doc;
        }
        output.property(config_.docid_property_name) = uuid;
        SetItemCount_(output, all_docid_list.size());
        output.property(config_.price_property_name) = price.ToUString();
        op_processor_->Append(2, output);
    }
    
    //update DM and IM then
    if(!data_source_->UpdateUuid(docid_list, uuid))
    {
        error_ = "Update uuid failed";
        return false;
    }
    return true;
}

bool ProductManager::AppendToGroup(const izenelib::util::UString& uuid, const std::vector<uint32_t>& docid_list, bool backup)
{
    if(inhook_)
    {
        error_ = "In Hook locks, collection was indexing, plz wait.";
        return false;
    }
    boost::mutex::scoped_lock lock(human_mutex_);
    std::cout<<"ProductManager::AppendToGroup"<<std::endl;
    std::vector<uint32_t> uuid_docid_list;
    data_source_->GetDocIdList(uuid, uuid_docid_list, 0);
    if(uuid_docid_list.empty())
    {
        std::string suuid;
        uuid.convertString(suuid, izenelib::util::UString::UTF_8);
        error_ = suuid+" not exists";
        return false;
    }
    if(!AppendToGroup_(uuid, uuid_docid_list, docid_list, PMDocumentType()))
    {
        return false;
    }
    if(!GenOperations())
    {
        return false;
    }
    if(backup && backup_)
    {
        BackupPCItem_(uuid, docid_list, 1);
    }
    return true;
}
    
bool ProductManager::RemoveFromGroup(const izenelib::util::UString& uuid, const std::vector<uint32_t>& docid_list, bool backup)
{
    if(inhook_)
    {
        error_ = "In Hook locks, collection was indexing, plz wait.";
        return false;
    }
    boost::mutex::scoped_lock lock(human_mutex_);
    std::cout<<"ProductManager::RemoveFromGroup"<<std::endl;
    if(docid_list.empty()) 
    {
        error_ = "Docid list size must larger than 0";
        return false;
    }
    std::vector<uint32_t> uuid_docid_list;
    data_source_->GetDocIdList(uuid, uuid_docid_list, 0);
    if(uuid_docid_list.empty())
    {
        std::string suuid;
        uuid.convertString(suuid, izenelib::util::UString::UTF_8);
        error_ = suuid+" not exists";
        return false;
    }
    boost::unordered_set<uint32_t> contains;
    for(uint32_t i=0;i<uuid_docid_list.size();i++)
    {
        contains.insert(uuid_docid_list[i]);
    }
    for(uint32_t i=0;i<docid_list.size();i++)
    {
        if( contains.find(docid_list[i]) == contains.end() )
        {
            error_ = "Document "+boost::lexical_cast<std::string>(docid_list[i])+" not in specific uuid";
            return false;
        }
        contains.erase(docid_list[i]);
    }
    std::vector<uint32_t> remain;
    boost::unordered_set<uint32_t>::iterator it = contains.begin();
    while( it!=contains.end())
    {
        remain.push_back(*it);
        ++it;
    }
    std::vector<PMDocumentType> doc_list(docid_list.size());
    for(uint32_t i=0;i<docid_list.size();i++)
    {
        if(!data_source_->GetDocument(docid_list[i], doc_list[i]))
        {
            error_ = "Can not get document "+boost::lexical_cast<std::string>(docid_list[i]);
            return false;
        }
    }
    
    if( remain.empty())
    {
        PMDocumentType del_doc;
        del_doc.property(config_.docid_property_name) = uuid;
        op_processor_->Append(3, del_doc);
    }
    else
    {
        ProductPrice price;
        GetPrice_(remain, price);
        //validation finished here.
        
        PMDocumentType update_doc;
        update_doc.property(config_.docid_property_name) = uuid;
        SetItemCount_(update_doc, remain.size());
        update_doc.property(config_.price_property_name) = price.ToUString();
        op_processor_->Append(2, update_doc);
    }
    std::vector<izenelib::util::UString> uuid_list(doc_list.size());
    for(uint32_t i=0;i<doc_list.size();i++)
    {
        //TODO need to be more strong here.
        uuid_list[i] = uuid_gen_->Gen(doc_list[i]);
        doc_list[i].property(config_.docid_property_name) = uuid_list[i];
        SetItemCount_(doc_list[i], doc_list.size());
        doc_list[i].eraseProperty(config_.uuid_property_name);
        op_processor_->Append(1, doc_list[i]);
    }
    if(!GenOperations())
    {
        return false;
    }
    if(backup && backup_)
    {
        BackupPCItem_(uuid, docid_list, 2);
    }
    //update DM and IM here
    for(uint32_t i=0;i<docid_list.size();i++)
    {
        std::vector<uint32_t> tmp_list(1, docid_list[i]);
        if(!data_source_->UpdateUuid(tmp_list, uuid_list[i]))
        {
            //TODO how to rollback?
        }
    }
    return true;
}

bool ProductManager::GetPrice_(uint32_t docid, ProductPrice& price)
{
    PMDocumentType doc;
    if(!data_source_->GetDocument(docid, doc)) return false;
    return GetPrice_(doc, price);
}

bool ProductManager::GetPrice_(const PMDocumentType& doc, ProductPrice& price)
{
    Document::property_const_iterator it = doc.findProperty(config_.price_property_name);
    if(it == doc.propertyEnd())
    {
        return false;
    }
    izenelib::util::UString price_str = it->second.get<izenelib::util::UString>();
    if(!price.Parse(price_str)) return false;
    return true;
}

void ProductManager::GetPrice_(const std::vector<uint32_t>& docid_list, ProductPrice& price)
{
    for(uint32_t i=0;i<docid_list.size();i++)
    {
        ProductPrice p;
        if(GetPrice_(docid_list[i], p))
        {
            price += p;
        }
    }
}

bool ProductManager::GetUuid_(const PMDocumentType& doc, izenelib::util::UString& uuid)
{
    PMDocumentType::property_const_iterator it = doc.findProperty(config_.uuid_property_name);
    if(it == doc.propertyEnd())
    {
        return false;
    }
    uuid = it->second.get<izenelib::util::UString>();
    return true;
}

bool ProductManager::GetDOCID_(const PMDocumentType& doc, izenelib::util::UString& docid)
{
    PMDocumentType::property_const_iterator it = doc.findProperty(config_.docid_property_name);
    if(it == doc.propertyEnd())
    {
        return false;
    }
    docid = it->second.get<izenelib::util::UString>();
    return true;
}

void ProductManager::SetItemCount_(PMDocumentType& doc, uint32_t item_count)
{
    doc.property(config_.itemcount_property_name) = izenelib::util::UString(boost::lexical_cast<std::string>(item_count), izenelib::util::UString::UTF_8);
}


